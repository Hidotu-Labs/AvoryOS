#!/usr/bin/env python3
"""Exercise real LinuxKPI transaction locks with the native wake rendezvous.

Checks single-lock contention, backoff accounting, opposite-order BO locking,
and wound delivery while the holder is blocked on another lock. Old source
can be passed as argv[1] to verify that the regression rejects it.
"""
from pathlib import Path
import ast
import re
import subprocess
import sys
import tempfile

root = Path(__file__).resolve().parents[2]
tree = ast.parse((root / 'scripts/tests/test_kpi_mutex_contention.py').read_text())
pre = next(ast.literal_eval(n.value) for n in tree.body
           if isinstance(n, ast.Assign) and any(isinstance(t, ast.Name) and t.id == 'code' for t in n.targets))
pre = pre.replace('#include <stdio.h>', '#include <stdio.h>\n#include <errno.h>\n#include <unistd.h>')
pre += '#include <stdint.h>\n'
pre = pre.replace('bool pending;', 'bool pending; bool signaled;')
pre = pre.replace('(void)t;return false;', 'return t->signaled;')
pre = pre.replace('static void linuxkpi_thread_block(void) {', '''
static struct mutex *audit_lock;
static struct task_struct *requeue_task;
static atomic_int requeue_hold;
static void audit_free_head_sleep(void);
static void linuxkpi_thread_block(void) {
 audit_free_head_sleep();
 while(current==requeue_task && atomic_load(&requeue_hold))sched_yield();''')
pre += r'''
#define spin_lock_irqsave(l,f) do {(f)=0;spin_lock(l);}while(0)
#define spin_unlock_irqrestore(l,f) do {(void)(f);spin_unlock(l);}while(0)
#define list_entry(p,t,m) ((t*)((char*)(p)-offsetof(t,m)))
#define list_for_each_entry(p,h,m) \
 for((p)=list_entry((h)->next,__typeof__(*(p)),m);&(p)->m!=(h); \
     (p)=list_entry((p)->m.next,__typeof__(*(p)),m))
'''
header = (root / 'kernel/linuxkpi/include/linux/ww_mutex.h').read_text()
header = re.sub(r'^#include.*\n', '', header, flags=re.M)
mutex = (root / 'kernel/linuxkpi/src/mutex.c').read_text()
mutex = re.sub(r'^#include.*\n', '', mutex, flags=re.M)
ww = Path(sys.argv[1]).read_text() if len(sys.argv) > 1 else (root / 'kernel/linuxkpi/src/ww_mutex.c').read_text()
ww = re.sub(r'^#include.*\n', '', ww, flags=re.M)
pre += '#define HAS_WW_QUEUE ' + str(int('struct kpi_ww_waiter' in ww)) + '\n'
post = r'''
static void audit_free_head_sleep(void) {
#if HAS_WW_QUEUE
 if(!audit_lock || current==requeue_task)return;
 spin_lock(&audit_lock->wait_lock);
 if(!audit_lock->owner && !list_empty(&audit_lock->wait_list)) {
  struct kpi_ww_waiter *head=list_first_entry(&audit_lock->wait_list,struct kpi_ww_waiter,list);
  /* With the other waiter deliberately parked, nobody can change owner
   * between the actual ww implementation's check and its decision to sleep. */
  assert(head->task!=current && "first waiter slept with owner=0");
 }
 spin_unlock(&audit_lock->wait_lock);
#endif
}
static void task_init(void) {
 pthread_mutex_init(&task.lock,NULL);pthread_cond_init(&task.cond,NULL);
 task.pending=false;task.signaled=false;
}
static struct ww_mutex locks[2];
static struct ww_class cls;
static atomic_int ready, release_holder, done, backs, protected;
static void *requeue_waiter(void *unused) {
 (void)unused;task_init();struct ww_acquire_ctx c;ww_acquire_init(&c,&cls);
 requeue_task=current; /* Read by this same thread's block hook. */
 assert(!ww_mutex_lock(&locks[0],&c));ww_mutex_unlock(&locks[0]);
 assert(!c.acquired);return NULL;
}
static void test_requeue_free_lock(void) {
 struct ww_acquire_ctx older;ww_acquire_init(&older,&cls);
 assert(!ww_mutex_lock(&locks[0],&older));
 audit_lock=&locks[0].base;atomic_store(&requeue_hold,1);
 pthread_t thread;pthread_create(&thread,NULL,requeue_waiter,NULL);
 for(;;) {
  spin_lock(&locks[0].base.wait_lock);
  bool queued=!list_empty(&locks[0].base.wait_list);
  spin_unlock(&locks[0].base.wait_lock);
  if(queued)break;
  sched_yield();
 }
 /* Release B's lock but prevent B consuming its wake. Reacquiring with
  * our older ticket puts us ahead of B while owner is NULL. */
 ww_mutex_unlock(&locks[0]);
 assert(!ww_mutex_lock(&locks[0],&older));
 ww_mutex_unlock(&locks[0]);atomic_store(&requeue_hold,0);
 pthread_join(thread,NULL);audit_lock=NULL;requeue_task=NULL;
 assert(!older.acquired);
}
static atomic_int rapid_count;
static void *rapid_worker(void *unused) {
 (void)unused;task_init();struct ww_acquire_ctx c;ww_acquire_init(&c,&cls);
 for(int i=0;i<1500;i++) {
  assert(!ww_mutex_lock(&locks[0],&c));
  assert(atomic_fetch_add(&protected,1)==0);
  atomic_fetch_add(&rapid_count,1);
  assert(atomic_fetch_sub(&protected,1)==1);
  ww_mutex_unlock(&locks[0]);assert(c.acquired==0);
 }
 return NULL;
}
static void *first_holder(void *unused) {
 (void)unused;task_init();struct ww_acquire_ctx c;ww_acquire_init(&c,&cls);
 assert(!ww_mutex_lock(&locks[0],&c));atomic_store(&ready,1);
 while(!atomic_load(&release_holder))sched_yield();
 ww_mutex_unlock(&locks[0]);assert(c.acquired==0);return NULL;
}
static void *first_waiter(void *unused) {
 (void)unused;task_init();struct ww_acquire_ctx c;ww_acquire_init(&c,&cls);
 while(!atomic_load(&ready))sched_yield();
 /* No other BO held: this acquisition must wait, never return EDEADLK. */
 assert(!ww_mutex_lock(&locks[0],&c));assert(c.acquired==1);
 ww_mutex_unlock(&locks[0]);assert(c.acquired==0);atomic_store(&done,1);return NULL;
}
static pthread_barrier_t opposite;
static void *opposite_worker(void *arg) {
 unsigned first=(uintptr_t)arg;task_init();
 for(int round=0;round<1000;round++) {
  struct ww_acquire_ctx c;ww_acquire_init(&c,&cls);
  assert(!ww_mutex_lock(&locks[first],&c));
  pthread_barrier_wait(&opposite);
  unsigned held=first,target=1-first;
  int r;
  while((r=ww_mutex_lock(&locks[target],&c))==-EDEADLK) {
   atomic_fetch_add(&backs,1);ww_mutex_unlock(&locks[held]);assert(c.acquired==0);
   assert(!ww_mutex_lock_slow(&locks[target],&c));
   unsigned swap=held;held=target;target=swap;
  }
  assert(!r);
  assert(c.acquired==2);assert(atomic_fetch_add(&protected,1)==0);
  atomic_fetch_sub(&protected,1);
  ww_mutex_unlock(&locks[0]);ww_mutex_unlock(&locks[1]);assert(c.acquired==0);
  /* Stop the next round's initial acquires until both released this pair. */
  pthread_barrier_wait(&opposite);
 }
 return NULL;
}
/* Force a younger holder to block first, then wound it from an older task.
 * The younger must wake to drop B while the older is blocked on B. */
static struct ww_acquire_ctx old_ctx;
static atomic_int old_acquired, young_acquired, start_old;
static void *older_worker(void *unused) {
 (void)unused;task_init();ww_acquire_init(&old_ctx,&cls);
 assert(!ww_mutex_lock(&locks[0],&old_ctx));atomic_store(&old_acquired,1);
 while(!atomic_load(&start_old))sched_yield();
 assert(!ww_mutex_lock(&locks[1],&old_ctx));
 ww_mutex_unlock(&locks[1]);ww_mutex_unlock(&locks[0]);assert(old_ctx.acquired==0);
 return NULL;
}
static void *younger_worker(void *unused) {
 (void)unused;task_init();while(!atomic_load(&old_acquired))sched_yield();
 struct ww_acquire_ctx c;ww_acquire_init(&c,&cls);
 assert(!ww_mutex_lock(&locks[1],&c));atomic_store(&young_acquired,1);
 int r=ww_mutex_lock(&locks[0],&c);assert(r==-EDEADLK);
 ww_mutex_unlock(&locks[1]);assert(c.acquired==0);
 assert(!ww_mutex_lock_slow(&locks[0],&c));assert(!c.wounded);
 assert(!ww_mutex_lock(&locks[1],&c));
 ww_mutex_unlock(&locks[0]);ww_mutex_unlock(&locks[1]);assert(c.acquired==0);
 return NULL;
}
int main(void) {
 task_init();pthread_t a,b;
 cls=(struct ww_class){.is_wait_die=true};
 ww_mutex_init(&locks[0],&cls);ww_mutex_init(&locks[1],&cls);
 pthread_create(&a,NULL,first_holder,NULL);
 while(!atomic_load(&ready))sched_yield();
 pthread_create(&b,NULL,first_waiter,NULL);usleep(10000);
 atomic_store(&release_holder,1);pthread_join(a,NULL);pthread_join(b,NULL);
 assert(atomic_load(&done));
 for(int algorithm=0;algorithm<2;algorithm++) {
  cls.is_wait_die=algorithm;
  test_requeue_free_lock();
  pthread_t racers[8];atomic_store(&rapid_count,0);
  for(int i=0;i<8;i++)pthread_create(&racers[i],NULL,rapid_worker,NULL);
  for(int i=0;i<8;i++)pthread_join(racers[i],NULL);
  assert(atomic_load(&rapid_count)==12000);
 }
 struct ww_acquire_ctx c;ww_acquire_init(&c,&cls);
 assert(ww_mutex_trylock(&locks[0],&c)==1);
 assert(ww_mutex_lock(&locks[0],&c)==-EALREADY);
 assert(ww_mutex_trylock(&locks[0],&c)==0);assert(c.acquired==1);
 ww_mutex_unlock(&locks[0]);assert(c.acquired==0);
 assert(!ww_mutex_lock(&locks[0],NULL));task.signaled=true;
 assert(ww_mutex_lock_interruptible(&locks[0],&c)==-ERESTARTSYS);
 task.signaled=false;ww_mutex_unlock(&locks[0]);assert(!c.acquired);
 pthread_barrier_init(&opposite,NULL,2);
 for(int algorithm=0;algorithm<2;algorithm++) {
  cls.is_wait_die=algorithm;atomic_store(&backs,0);
  pthread_create(&a,NULL,opposite_worker,(void*)0);
  pthread_create(&b,NULL,opposite_worker,(void*)1);
  pthread_join(a,NULL);pthread_join(b,NULL);assert(atomic_load(&backs)>0);
 }
 cls.is_wait_die=false;
 pthread_create(&a,NULL,older_worker,NULL);pthread_create(&b,NULL,younger_worker,NULL);
 while(!atomic_load(&young_acquired))sched_yield();
 usleep(10000);atomic_store(&start_old,1);
 pthread_join(a,NULL);pthread_join(b,NULL);
 assert(!ww_mutex_is_locked(&locks[0])&&!ww_mutex_is_locked(&locks[1]));
 assert(list_empty(&locks[0].base.wait_list)&&list_empty(&locks[1].base.wait_list));
 puts("PASS: free-owner requeue, 24000 rapid 8-thread reacquisitions, first-lock contention, balanced BO refs, 2000 ABBA transactions, blocked-holder wound, slow retries, duplicate/trylock/signal semantics");
}
'''
with tempfile.TemporaryDirectory(prefix='avory-ww-') as temp:
    path = Path(temp)
    (path / 'check.c').write_text(re.sub(r'\btask\b', 'test_task', pre) + header + mutex + ww +
                                 re.sub(r'(?<![.>])\btask\b', 'test_task', post))
    subprocess.run(['cc', '-O2', '-pthread', '-Wall', '-Wextra', '-Werror',
                    str(path / 'check.c'), '-o', str(path / 'check')], check=True)
    subprocess.run([str(path / 'check')], check=True, timeout=25)
