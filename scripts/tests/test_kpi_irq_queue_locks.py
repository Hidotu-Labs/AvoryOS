#!/usr/bin/env python3
"""Inject a same-CPU hard IRQ just after real LinuxKPI lock acquisition."""
from pathlib import Path
import re, subprocess, tempfile
root = Path(__file__).resolve().parents[2]
def function(source, signature):
 start=source.index(signature);end=source.index('{',start)+1;depth=1
 while depth:
  depth+=(source[end]=='{')-(source[end]=='}');end+=1
 return source[start:end]
def strip_includes(s):
 return re.sub(r'^#include.*$', '', s, flags=re.M)
spin_types=strip_includes((root/'kernel/linuxkpi/include/linux/spinlock_types.h').read_text())
spin=strip_includes((root/'kernel/linuxkpi/include/linux/spinlock.h').read_text())
service=strip_includes((root/'kernel/linuxkpi/src/service.c').read_text())
wq=(root/'kernel/linuxkpi/src/workqueue.c').read_text()
wq='\n'.join(function(wq,x) for x in ['static void wq_ensure_worker(', 'static bool __queue_work(', 'bool cancel_work_sync('])
kt=(root/'kernel/linuxkpi/src/kthread.c').read_text()
kt='\n'.join(function(kt,x) for x in ['static void kthread_worker_spawn(', 'bool kthread_queue_work(', 'bool kthread_cancel_work_sync('])
pre=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <stdio.h>
static bool irq_enabled=true, armed, irq_pending, in_irq;
static int preempt_depth, interrupts, wakes;
static void irq_handler(void);
static void deliver(void){
 if(!irq_enabled||!irq_pending)return;
 assert(!in_irq);irq_pending=false;irq_enabled=false;in_irq=true;interrupts++;
 irq_handler();in_irq=false;irq_enabled=true;
}
static unsigned mock_exchange(volatile unsigned *p,unsigned v,int order){
 unsigned old=__atomic_exchange_n(p,v,order);
 assert(!(old && in_irq) && "same-CPU IRQ reacquired its interrupted holder's lock");
 if(!old&&armed){armed=false;irq_pending=true;deliver();}
 return old;
}
static unsigned long save_irq(void){unsigned long f=irq_enabled?512:0;irq_enabled=false;return f;}
static void restore_irq(unsigned long f){irq_enabled=!!(f&512);deliver();}
#define local_irq_save(f) ((f)=save_irq())
#define local_irq_restore(f) restore_irq(f)
#define local_irq_disable() (irq_enabled=false)
#define local_irq_enable() restore_irq(512)
#define preempt_disable() (++preempt_depth)
#define preempt_enable() do{assert(preempt_depth>0);--preempt_depth;}while(0)
#define preempt_enable_resched() preempt_enable()
void linuxkpi_tlb_poll(void){}
#undef __always_inline
#define __always_inline inline
#define __atomic_exchange_n(p,v,o) mock_exchange(p,v,o)
'''
shim=r'''
#undef __atomic_exchange_n
#define GFP_KERNEL 0
#define IS_ERR(t) ((uintptr_t)(t)>=(uintptr_t)-4095)
#define KPI_WQ_MAX_WORKERS 4
#define kfree free
#define kmalloc(n,g) malloc(n)
#define wait_event(w,c) assert(c)
struct list_head{struct list_head *next,*prev;};
#define container_of(p,t,m) ((t*)((char*)(p)-offsetof(t,m)))
#define list_first_entry(h,t,m) container_of((h)->next,t,m)
static void INIT_LIST_HEAD(struct list_head *h){h->next=h->prev=h;}
static bool list_empty(struct list_head *h){return h->next==h;}
static void list_add_tail(struct list_head *p,struct list_head *h){p->next=h;p->prev=h->prev;h->prev->next=p;h->prev=p;}
static void list_del_init(struct list_head *p){p->prev->next=p->next;p->next->prev=p->prev;INIT_LIST_HEAD(p);}
struct wait_queue_head{int dummy;};
struct completion{int dummy;};
static void reinit_completion(struct completion *c){(void)c;}
static void wait_for_completion(struct completion *c){(void)c;assert(!in_irq);}
struct task_struct{int dummy;};
static struct task_struct tasks[2000];static unsigned task_count;
static struct task_struct *kthread_create(int(*fn)(void*),void *arg,const char *fmt,...){(void)fn;(void)arg;(void)fmt;assert(task_count<2000);return &tasks[task_count++];}
static void kthread_detach(struct task_struct *t){assert(t);}
static void wake_up_process(struct task_struct *t){assert(t);wakes++;}
static int worker_fn(void *a){(void)a;return 0;}
static int kthread_worker_fn(void *a){(void)a;return 0;}
static void __kpi_wake_up(struct wait_queue_head *w,int n,int k){(void)w;(void)n;(void)k;}
static void wake_up(struct wait_queue_head *w){(void)w;}
struct workqueue_struct;
struct work_struct{struct list_head entry;void(*func)(struct work_struct*);int pending,running;struct workqueue_struct *wq;};
struct workqueue_struct{char *name;struct list_head pending;spinlock_t lock;struct wait_queue_head more_work,flush_wait;unsigned active,gen;int max_workers;unsigned nr_workers;struct task_struct *workers[4];};
struct wq_worker{struct workqueue_struct *wq;int idx;};
struct kthread_worker;
struct kthread_work{struct list_head node;void(*func)(struct kthread_work*);struct kthread_worker *worker;struct completion done;int queued;};
struct kthread_worker{struct task_struct *task;spinlock_t lock;struct list_head work_list;struct kthread_work *current_work;struct wait_queue_head wait,wait_idle;int should_stop;char name[16];};
'''
service_h=strip_includes((root/'kernel/linuxkpi/include/linuxkpi/service.h').read_text())
post=r'''
static struct kpi_service svc;
static struct workqueue_struct queue;
static struct work_struct outer_work,irq_work;
static struct kthread_worker kw;
static struct kthread_work outer_kw,irq_kw;
static int mode;
static int service_fn(void *p){(void)p;return 0;}
static void irq_handler(void){
 assert(!irq_enabled&&in_irq);
 if(mode<3)kpi_service_kick(&svc,true);
 else if(mode<6)assert(__queue_work(&queue,&irq_work));
 else assert(kthread_queue_work(&kw,&irq_kw));
}
static void setup(int m){
 mode=m;irq_pending=false;armed=false;preempt_depth=0;
 kpi_service_init(&svc,"test",service_fn,NULL);svc.task=&tasks[0];
 queue=(struct workqueue_struct){.name="test",.max_workers=1,.nr_workers=1};
 queue.workers[0]=&tasks[0];spin_lock_init(&queue.lock);INIT_LIST_HEAD(&queue.pending);
 outer_work=(struct work_struct){.wq=&queue};irq_work=(struct work_struct){.wq=&queue};
 INIT_LIST_HEAD(&outer_work.entry);INIT_LIST_HEAD(&irq_work.entry);
 kw=(struct kthread_worker){.task=&tasks[0]};spin_lock_init(&kw.lock);INIT_LIST_HEAD(&kw.work_list);
 outer_kw=(struct kthread_work){.worker=&kw};irq_kw=(struct kthread_work){.worker=&kw};
 INIT_LIST_HEAD(&outer_kw.node);INIT_LIST_HEAD(&irq_kw.node);
 if(m==5){outer_work.pending=1;queue.active=1;list_add_tail(&outer_work.entry,&queue.pending);}
 if(m==8){outer_kw.queued=1;list_add_tail(&outer_kw.node,&kw.work_list);}
}
int main(void){
 for(int cycle=0;cycle<100;cycle++)for(int m=0;m<9;m++)for(int enabled=0;enabled<2;enabled++){
  setup(m);irq_enabled=enabled;armed=true;int before=interrupts;
  switch(m){
   case 0:kpi_service_kick(&svc,true);break;
   case 1:assert(kpi_service_retire(&svc,0));break;
   case 2:kpi_service_forget(&svc);break;
   case 3:assert(__queue_work(&queue,&outer_work));break;
   case 4:wq_ensure_worker(&queue);break;
   case 5:assert(cancel_work_sync(&outer_work));break;
   case 6:assert(kthread_queue_work(&kw,&outer_kw));break;
   case 7:kthread_worker_spawn(&kw);break;
   case 8:assert(kthread_cancel_work_sync(&outer_kw));break;
  }
  assert(irq_enabled==(bool)enabled&&preempt_depth==0&&!armed);
  if(!enabled){assert(interrupts==before&&irq_pending);restore_irq(512);}
  assert(interrupts==before+1&&!irq_pending&&preempt_depth==0);
  if(m<3){assert(svc.task&&svc.gen==(m==0?2u:1u));}
  else if(m<6){assert(irq_work.pending&&!irq_work.running);assert(queue.active==(m==3?2u:1u));}
  else{assert(irq_kw.queued&&kw.task);}
 }
 puts("PASS: 1800 same-CPU IRQ injections; service/queue/spawn/retire/cancel preserve IRQ state and published work");
}
'''
def plain(s):
 s=re.sub(r'spin_lock_irqsave\(([^,]+), flags\);',r'spin_lock(\1);',s)
 return re.sub(r'spin_unlock_irqrestore\(([^,]+), flags\);',r'spin_unlock(\1);',s)
with tempfile.TemporaryDirectory(prefix='kpi-irq-locks-') as d:
 p=Path(d)
 variants=[('fixed',service,wq,kt),('service plain lock',plain(service),wq,kt),('workqueue plain lock',service,plain(wq),kt),('kthread worker plain lock',service,wq,plain(kt))]
 for name,a,b,c in variants:
  (p/'test.c').write_text(pre+spin_types+spin+shim+service_h+a+b+c+post)
  subprocess.run(['cc','-std=gnu11','-O2','-Wall','-Wextra','-Werror','-Wno-unused-function','-Wno-unused-variable',str(p/'test.c'),'-o',str(p/'test')],check=True)
  result=subprocess.run([str(p/'test')],capture_output=True,text=True,timeout=5)
  if name=='fixed':
   assert result.returncode==0,result.stderr;print(result.stdout.strip())
  else:
   assert result.returncode!=0 and 'same-CPU IRQ reacquired' in result.stderr,(name,result)
   print(f'PASS: {name} reproduces the interrupt self-deadlock')
