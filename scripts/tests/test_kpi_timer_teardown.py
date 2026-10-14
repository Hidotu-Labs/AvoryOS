#!/usr/bin/env python3
"""Exercise real timer expiry blocks and cancellation at callback/rearm races."""
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[2]
s=(root/'kernel/linuxkpi/src/timer.c').read_text()
def fn(sig):
 a=s.index(sig);end=s.index('{',a)+1;depth=1
 while depth:
  depth+=(s[end]=='{')-(s[end]=='}');end+=1
 return s[a:end]+'\n'
a=s.index('    for (;;) {\n      spin_lock_irqsave(&timer_lock, flags);',s.index('static int timer_kthread'))
b=s.index('    if (wait_ms == 0)',a)
block=s[a:b]
code=r'''
#define _DEFAULT_SOURCE
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <pthread.h>
#include <stdatomic.h>
#include <unistd.h>
#include <stdio.h>
struct list_head {struct list_head *next,*prev;};
#define container_of(p,t,m) ((t*)((char*)(p)-offsetof(t,m)))
#define list_entry(p,t,m) container_of(p,t,m)
#define list_first_entry(p,t,m) list_entry((p)->next,t,m)
static void init_list(struct list_head*h){h->next=h->prev=h;}
static bool list_empty(const struct list_head*h){return h->next==h;}
static void list_add(struct list_head*n,struct list_head*h){n->next=h->next;n->prev=h;h->next->prev=n;h->next=n;}
static void list_del_init(struct list_head*n){n->prev->next=n->next;n->next->prev=n->prev;init_list(n);}
#define msleep(n) usleep((n)*1000)
#define spin_lock_irqsave(l,f) do{(f)=0;pthread_mutex_lock(l);}while(0)
#define spin_unlock_irqrestore(l,f) do{(void)(f);pthread_mutex_unlock(l);}while(0)
static pthread_mutex_t timer_lock=PTHREAD_MUTEX_INITIALIZER;
static struct list_head timer_wheel,hrtimer_wheel,expired_timers,expired_hrtimers;
static int timer_done_wq;
static void __kpi_wake_up(int*p,int a,int b){(void)p;(void)a;(void)b;}
static void timer_signal_change(void){}
static bool irqs_disabled(void){return false;}
static bool in_atomic(void){return false;}
typedef int64_t ktime_t;
static ktime_t ktime_get(void){return 0;}
enum hrtimer_restart {HRTIMER_NORESTART,HRTIMER_RESTART};
struct timer_list {struct list_head entry;unsigned long expires;void(*function)(struct timer_list*);int running;};
struct hrtimer {struct {struct list_head node;ktime_t expires;}node;enum hrtimer_restart(*function)(struct hrtimer*);int queued,running,cancel_pending;};
'''+''.join(fn(sig) for sig in ['int timer_pending(', 'int del_timer(', 'int del_timer_sync(', 'static void __hrtimer_enqueue_locked(', 'int hrtimer_try_to_cancel(', 'int hrtimer_cancel('])+ '\nstatic void drain(void){unsigned long flags;\n'+block+'}\n'+r'''
static struct timer_list timer;static struct hrtimer hr;
static atomic_bool entered,release_cb,cancel_done;
static void cb(struct timer_list*t){
 atomic_store(&entered,true);while(!atomic_load(&release_cb))usleep(50);
 unsigned long f;spin_lock_irqsave(&timer_lock,f);list_add(&t->entry,&timer_wheel);spin_unlock_irqrestore(&timer_lock,f);
}
static enum hrtimer_restart hcb(struct hrtimer*h){(void)h;atomic_store(&entered,true);while(!atomic_load(&release_cb))usleep(50);return HRTIMER_RESTART;}
static void *worker(void*p){(void)p;drain();return NULL;}
static void *cancel(void*p){if(p)hrtimer_cancel(&hr);else del_timer_sync(&timer);atomic_store(&cancel_done,true);return NULL;}
int main(void){
 init_list(&timer_wheel);init_list(&hrtimer_wheel);init_list(&expired_timers);init_list(&expired_hrtimers);
 init_list(&timer.entry);timer.function=cb;list_add(&timer.entry,&expired_timers);
 /* Cancellation may remove an expired batch item before execution. */
 assert(del_timer_sync(&timer)==1);drain();assert(!atomic_load(&entered));
 for(int high=0;high<2;high++){
  atomic_store(&entered,false);atomic_store(&release_cb,false);atomic_store(&cancel_done,false);
  if(high){init_list(&hr.node.node);hr.function=hcb;hr.queued=1;list_add(&hr.node.node,&expired_hrtimers);}
  else list_add(&timer.entry,&expired_timers);
  pthread_t wt,ct;assert(!pthread_create(&wt,NULL,worker,NULL));while(!atomic_load(&entered))usleep(50);
  if(high)assert(hrtimer_try_to_cancel(&hr)==-1);
  assert(!pthread_create(&ct,NULL,cancel,(void*)(uintptr_t)high));
  usleep(20000);assert(!atomic_load(&cancel_done));atomic_store(&release_cb,true);
  pthread_join(wt,NULL);pthread_join(ct,NULL);
  assert(!timer.running&&!hr.running&&!hr.queued&&list_empty(&timer.entry));
  assert(list_empty(&timer_wheel)&&list_empty(&hrtimer_wheel));
 }
 puts("PASS: expired batch cancellation, running timer rearm drain, hrtimer restart/cancel and try-cancel active contract");
}
'''
with tempfile.TemporaryDirectory(prefix='kpi-timer-teardown-') as tmp:
 p=Path(tmp);(p/'test.c').write_text(code)
 subprocess.run(['cc','-std=gnu11','-O1','-g','-pthread','-fsanitize=address,undefined',str(p/'test.c'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True,timeout=10)
 # Prove the test catches deletion that checks only the running bit and
 # misses a callback's rearm while the caller waits.
 old = """int del_timer_sync(struct timer_list *timer) {
  int ret = del_timer(timer);
  while (__atomic_load_n(&timer->running, __ATOMIC_ACQUIRE)) msleep(1);
  return ret;
 }"""
 (p/'old.c').write_text(code.replace(fn('int del_timer_sync('),old))
 subprocess.run(['cc','-std=gnu11','-O1','-pthread',str(p/'old.c'),'-o',str(p/'old')],check=True)
 result=subprocess.run([str(p/'old')],capture_output=True,timeout=10)
 assert result.returncode != 0, 'former del_timer_sync unexpectedly passed'
 print('PASS: former del_timer_sync fails the callback-rearm regression')
