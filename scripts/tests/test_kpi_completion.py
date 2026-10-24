#!/usr/bin/env python3
"""Exercise actual completion/waitqueue code with native scheduling mocked.

--source permits a captured pre-fix completion.c negative control. The
two-waiter test parks both old unlocked reads after observing the same token;
the new implementation instead claims it under the queue lock.
"""
from pathlib import Path
import argparse
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--source', type=Path, default=ROOT/'kernel/linuxkpi/src/completion.c')
parser.add_argument('--case', choices=['all', 'zero', 'race', 'wake'], default='all')
args = parser.parse_args()

PRELUDE = r'''
#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <sched.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#define ERESTARTSYS 512
#define TASK_RUNNING 0
#define TASK_INTERRUPTIBLE 1
#define TASK_UNINTERRUPTIBLE 2
#define TASK_WAKEKILL 128
#define TASK_KILLABLE (TASK_UNINTERRUPTIBLE|TASK_WAKEKILL)
#define MAX_SCHEDULE_TIMEOUT LONG_MAX
struct list_head { struct list_head *next,*prev; };
#define LIST_HEAD_INIT(n) { &(n), &(n) }
static void INIT_LIST_HEAD(struct list_head *p) {p->next=p->prev=p;}
static bool list_empty(const struct list_head *p) {return p->next==p;}
static void list_add(struct list_head *p,struct list_head *h) {
 p->next=h->next;p->prev=h;h->next->prev=p;h->next=p;
}
static void list_add_tail(struct list_head *p,struct list_head *h) {
 p->prev=h->prev;p->next=h;h->prev->next=p;h->prev=p;
}
static void list_del_init(struct list_head *p) {
 p->next->prev=p->prev;p->prev->next=p->next;INIT_LIST_HEAD(p);
}
#define list_entry(p,t,m) ((t*)((char*)(p)-offsetof(t,m)))
#define list_for_each_entry_safe(p,n,h,m) \
 for(p=list_entry((h)->next,__typeof__(*p),m),n=list_entry(p->m.next,__typeof__(*p),m); \
 &p->m!=(h);p=n,n=list_entry(n->m.next,__typeof__(*n),m))
#define list_first_entry(h,t,m) ((t*)((char*)(h)->next - offsetof(t,m)))
typedef struct {pthread_mutex_t mutex;bool held;} spinlock_t;
static _Thread_local bool wake_requires_lock;
static spinlock_t *expected_wake_lock;
static int wake_count,expected_wake_count;
static void spin_lock_init(spinlock_t *p) {pthread_mutex_init(&p->mutex,NULL);p->held=false;}
static void spin_lock(spinlock_t *p) {pthread_mutex_lock(&p->mutex);p->held=true;}
static void spin_unlock(spinlock_t *p) {
 if(wake_requires_lock&&p==expected_wake_lock)assert(wake_count>expected_wake_count);
 p->held=false;pthread_mutex_unlock(&p->mutex);
}
#define spin_lock_irqsave(p,f) do {(f)=0;spin_lock(p);} while(0)
#define spin_unlock_irqrestore(p,f) do {(void)(f);spin_unlock(p);} while(0)
#define spin_lock_irq(p) spin_lock(p)
#define spin_unlock_irq(p) spin_unlock(p)
struct task_struct {pthread_mutex_t lock;pthread_cond_t cond;bool pending,signalled;int state;};
static _Thread_local struct task_struct task;
static _Thread_local bool task_ready;
static _Thread_local bool waiter_thread;
static pthread_barrier_t read_barrier;
static bool force_old_read_race;
static void setup_task(void) {
 if(task_ready)return;
 pthread_mutex_init(&task.lock,NULL);pthread_cond_init(&task.cond,NULL);
 task_ready=true;
}
#define current (&task)
static void *linuxkpi_current_thread(void) {setup_task();return &task;}
static void *task_struct_to_thread(struct task_struct *p) {return p;}
static bool signal_pending(struct task_struct *p) {return p->signalled;}
static void set_current_state(int state) {setup_task();task.state=state;}
static void __set_current_state(int state) {
 setup_task();task.state=state;
 if(state==TASK_RUNNING){pthread_mutex_lock(&task.lock);task.pending=false;pthread_mutex_unlock(&task.lock);}
}
static void linuxkpi_wake_thread(void *p) {
 struct task_struct *t=p;
 if(wake_requires_lock)assert(expected_wake_lock->held);
 __atomic_add_fetch(&wake_count,1,__ATOMIC_RELAXED);
 pthread_mutex_lock(&t->lock);t->pending=true;pthread_cond_signal(&t->cond);pthread_mutex_unlock(&t->lock);
}
static void linuxkpi_wake_poll_queue(void *p) {(void)p;}
#define DEFINE_SPINLOCK(name) spinlock_t name = {.mutex=PTHREAD_MUTEX_INITIALIZER}
#define GFP_ATOMIC 0
static void *kzalloc(size_t n,int flags) {(void)flags;return calloc(1,n);}
static void kfree(void *p) {free(p);}
static void asc_vfs_poll_notify(void *node,void *wq) {(void)node;(void)wq;}
static long schedule_timeout(long timeout) {
 setup_task();pthread_mutex_lock(&task.lock);
 struct timespec end;clock_gettime(CLOCK_REALTIME,&end);
 if(timeout!=MAX_SCHEDULE_TIMEOUT){
  end.tv_sec+=timeout/1000;end.tv_nsec+=(timeout%1000)*1000000;
  if(end.tv_nsec>=1000000000){end.tv_nsec-=1000000000;end.tv_sec++;}
 }
 int ret=0;
 while(!task.pending&&!task.signalled&&ret!=ETIMEDOUT) {
  if(timeout==MAX_SCHEDULE_TIMEOUT)ret=pthread_cond_wait(&task.cond,&task.lock);
  else ret=pthread_cond_timedwait(&task.cond,&task.lock,&end);
 }
 bool woken=task.pending;task.pending=false;pthread_mutex_unlock(&task.lock);
 if(timeout==MAX_SCHEDULE_TIMEOUT)return timeout;
 return woken||task.signalled ? (timeout>1?timeout-1:1) : 0;
}
static void schedule(void) {(void)schedule_timeout(MAX_SCHEDULE_TIMEOUT);}
static unsigned observed_load(unsigned *p,int order) {
 unsigned value=__atomic_load_n(p,order);
 if(force_old_read_race&&waiter_thread&&value==1)pthread_barrier_wait(&read_barrier);
 return value;
}
'''

TESTS = r'''
static struct completion shared;
static int returned;
static void *race_waiter(void *p) {
 (void)p;waiter_thread=true;setup_task();
 unsigned long ret=wait_for_completion_timeout(&shared,100);
 if(ret)__atomic_add_fetch(&returned,1,__ATOMIC_RELAXED);
 return NULL;
}
static void zero_case(void) {
 struct completion c;init_completion(&c);
 assert(wait_for_completion_timeout(&c,0)==0);
 complete(&c);
 assert(wait_for_completion_timeout(&c,0)==1 && !completion_done(&c));
 complete(&c);
 assert(wait_for_completion_interruptible_timeout(&c,0)==1 && !completion_done(&c));
 task.signalled=true;
 assert(wait_for_completion_interruptible_timeout(&c,0)==-ERESTARTSYS);
 complete(&c);
 assert(wait_for_completion_interruptible_timeout(&c,0)==1 && !completion_done(&c));
 task.signalled=false;
 puts("PASS: zero-time waits claim available completion and return success");
}
static void race_case(void) {
 init_completion(&shared);complete(&shared);returned=0;
 force_old_read_race=true;pthread_barrier_init(&read_barrier,NULL,2);
 pthread_t a,b;
 assert(!pthread_create(&a,NULL,race_waiter,NULL));
 assert(!pthread_create(&b,NULL,race_waiter,NULL));
 pthread_join(a,NULL);pthread_join(b,NULL);force_old_read_race=false;
 assert(returned==1 && shared.done==0);
 puts("PASS: two competing waiters consume exactly one completion token");
}
static void *infinite_waiter(void *p) {wait_for_completion(p);return NULL;}
static void wake_case(void) {
 struct completion c;init_completion(&c);pthread_t a;
 assert(!pthread_create(&a,NULL,infinite_waiter,&c));
 for(;;){spin_lock(&c.wait.lock);bool queued=!list_empty(&c.wait.head);spin_unlock(&c.wait.lock);if(queued)break;sched_yield();}
 expected_wake_lock=&c.wait.lock;expected_wake_count=wake_count;wake_requires_lock=true;
 complete(&c);wake_requires_lock=false;pthread_join(a,NULL);
 assert(!c.done && list_empty(&c.wait.head));
 assert(!pthread_create(&a,NULL,infinite_waiter,&c));
 for(;;){spin_lock(&c.wait.lock);bool queued=!list_empty(&c.wait.head);spin_unlock(&c.wait.lock);if(queued)break;sched_yield();}
 expected_wake_count=wake_count;wake_requires_lock=true;complete_all(&c);wake_requires_lock=false;pthread_join(a,NULL);
 assert(c.done==UINT_MAX && list_empty(&c.wait.head));
 puts("PASS: single/all completion wakeups run under the lifetime lock");
}
struct waiter_args {int kind;long ret;struct completion *c;bool signalled;};
static void *api_waiter(void *p) {
 struct waiter_args *a=p;setup_task();task.signalled=a->signalled;
 switch(a->kind){
 case 0:wait_for_completion(a->c);a->ret=1;break;
 case 1:a->ret=wait_for_completion_interruptible(a->c);break;
 case 2:a->ret=wait_for_completion_killable(a->c);break;
 case 3:a->ret=(long)wait_for_completion_timeout(a->c,1000);break;
 case 4:a->ret=wait_for_completion_interruptible_timeout(a->c,1000);break;
 }
 return NULL;
}
static void contracts_case(void) {
 struct completion c;init_completion(&c);
 for(int kind=0;kind<5;kind++){
  pthread_t t;struct waiter_args a={.kind=kind,.c=&c};
  assert(!pthread_create(&t,NULL,api_waiter,&a));
  for(;;){spin_lock(&c.wait.lock);bool queued=!list_empty(&c.wait.head);spin_unlock(&c.wait.lock);if(queued)break;sched_yield();}
  complete(&c);pthread_join(t,NULL);assert(a.ret==(kind==1||kind==2?0:1)|| (kind>=3&&a.ret>0));
  assert(!c.done && list_empty(&c.wait.head));
 }
 for(int kind=1;kind<5;kind++){
  if(kind==3)continue;
  pthread_t t;struct waiter_args a={.kind=kind,.c=&c,.signalled=true};
  assert(!pthread_create(&t,NULL,api_waiter,&a));pthread_join(t,NULL);
  assert(a.ret==-ERESTARTSYS && !c.done && list_empty(&c.wait.head));
  complete(&c);
  assert(!pthread_create(&t,NULL,api_waiter,&a));pthread_join(t,NULL);
  assert(a.ret==(kind==4?1000:0) && !c.done); // ready completion takes precedence
 }
 assert(wait_for_completion_timeout(&c,1)==0 && !completion_done(&c));
 complete_all(&c);
 for(int i=0;i<32;i++){assert(try_wait_for_completion(&c));assert(wait_for_completion_timeout(&c,0)==1);}
 assert(c.done==UINT_MAX);reinit_completion(&c);assert(!try_wait_for_completion(&c));
 // Counted tokens preserve payload publication through the common lock.
 for(int i=0;i<128;i++)complete(&c);
 for(int i=0;i<128;i++)assert(try_wait_for_completion(&c));
 assert(!try_wait_for_completion(&c));
 puts("PASS: all wait APIs, signals, timeout, complete_all and counted tokens");
}
static void *credit_waiter(void *p) {
 (void)p;
 for(int i=0;i<4000;i++){
  wait_for_completion(&shared);
  __atomic_add_fetch(&returned,1,__ATOMIC_RELAXED);
  if(!(i%17))sched_yield();
 }
 return NULL;
}
static void stress_case(void) {
 init_completion(&shared);returned=0;pthread_t workers[8];
 for(int i=0;i<8;i++)assert(!pthread_create(&workers[i],NULL,credit_waiter,NULL));
 for(int i=0;i<32000;i++){complete(&shared);if(!(i%19))sched_yield();}
 for(int i=0;i<8;i++)pthread_join(workers[i],NULL);
 assert(returned==32000 && shared.done==0 && list_empty(&shared.wait.head));
 puts("PASS: eight waiters consume 32000 signals with no early/lost credits");
}
int main(int argc,char **argv) {
 assert(argc==2);setup_task();
 if(!strcmp(argv[1],"zero")||!strcmp(argv[1],"all"))zero_case();
 if(!strcmp(argv[1],"race")||!strcmp(argv[1],"all"))race_case();
 if(!strcmp(argv[1],"wake")||!strcmp(argv[1],"all"))wake_case();
 if(!strcmp(argv[1],"all")){contracts_case();stress_case();}
 return 0;
}
'''

def stripped(path):
    return re.sub(r'^#include[^\n]*\n', '', path.read_text(), flags=re.MULTILINE)

with tempfile.TemporaryDirectory(prefix='avory-completion-') as temp:
    work=Path(temp)
    code=PRELUDE+stripped(ROOT/'kernel/linuxkpi/include/linux/wait.h')
    code+=stripped(ROOT/'kernel/linuxkpi/include/linux/completion.h')
    code+=stripped(ROOT/'kernel/linuxkpi/src/wait.c')
    # Instrument the old unlocked observation only; kernel implementation
    # and waitqueue macros are otherwise compiled unchanged.
    code+='\n#define __atomic_load_n(p,o) observed_load(p,o)\n'+stripped(args.source)
    code+='\n#undef __atomic_load_n\n'+TESTS
    (work/'check.c').write_text(code)
    exe=work/'check'
    subprocess.run(['cc','-std=gnu11','-O1','-g','-pthread','-Wall','-Wextra',
        '-Werror','-Wno-unused-function','-fsanitize=address,undefined',
        '-fno-omit-frame-pointer',work/'check.c','-o',exe],check=True)
    subprocess.run([exe,args.case],check=True,timeout=15)
