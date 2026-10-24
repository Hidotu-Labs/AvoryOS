#!/usr/bin/env python3
"""Real queue wake traversal, task conversion and stale-clock timeout accounting."""
from pathlib import Path
import subprocess
import tempfile
import re
root=Path(__file__).resolve().parents[2]
wait=(root/'kernel/linuxkpi/src/wait.c').read_text()
header=(root/'kernel/linuxkpi/include/linux/wait.h').read_text()
time=(root/'kernel/linuxkpi/src/time.c').read_text()
def function(source, signature):
 a=source.index(signature); b=source.index('{',a)+1; depth=1
 while depth:
  depth+=(source[b]=='{')-(source[b]=='}'); b+=1
 return source[a:b]+'\n'
code=r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <limits.h>
struct list_head { struct list_head *next,*prev; };
#define LIST_HEAD_INIT(h) {&(h),&(h)}
static void list_add(struct list_head *p,struct list_head *h) {
 p->next=h->next;p->prev=h;h->next->prev=p;h->next=p;
}
static void list_del_init(struct list_head *p) {
 p->prev->next=p->next;p->next->prev=p->prev;p->next=p->prev=p;
}
#define list_entry(p,t,m) ((t*)((char*)(p)-offsetof(t,m)))
#define list_for_each_entry_safe(p,n,h,m) \
 for(p=list_entry((h)->next,__typeof__(*p),m),n=list_entry(p->m.next,__typeof__(*p),m); \
 &p->m!=(h);p=n,n=list_entry(n->m.next,__typeof__(*n),m))
#define WQ_FLAG_EXCLUSIVE 1
struct task_struct { void *thread; };
static void *task_struct_to_thread(struct task_struct *p) { return p->thread; }
struct wait_queue_entry {
 unsigned flags;void *private;
 int (*func)(struct wait_queue_entry *,unsigned,int,void *);
 struct list_head entry;
};
typedef struct {struct list_head head;} wait_queue_head_t;
static void *woken_thread;
static void linuxkpi_wake_thread(void *p) { woken_thread=p; }
static unsigned long jiffies, now, elapsed;
static int reset, result;
#define TASK_RUNNING 0
#define MAX_SCHEDULE_TIMEOUT LONG_MAX
#define __set_current_state(s) (reset++)
static void linuxkpi_thread_block(void) {}
static void linuxkpi_jiffies_sync(void) { jiffies=now; }
static int linuxkpi_schedule_timeout_ms(unsigned long timeout) {
 assert(timeout==100);now+=elapsed;linuxkpi_jiffies_sync();return result;
}
'''
a=header.index('#define ___WAITQUEUE_INITIALIZER');b=header.index('#define __WAIT_QUEUE_HEAD_INITIALIZER',a)
code+=header[a:b]
for sig in ('int default_wake_function(', 'int autoremove_wake_function(', 'static int __kpi_wake_up_common_locked('):
 code+=function(wait,sig)
code+=function(time,'long schedule_timeout(')
code+=r'''
static int calls;
static int persistent(struct wait_queue_entry *e,unsigned m,int f,void *k) {
 (void)e;(void)m;(void)f;(void)k;calls++;return 1;
}
static int ignored(struct wait_queue_entry *e,unsigned m,int f,void *k) {
 (void)e;(void)m;(void)f;(void)k;return 0;
}
int main(void) {
 int native;
 struct task_struct task={&native};
 DECLARE_WAITQUEUE(autoentry,&task);
 assert(autoentry.private==&native && autoentry.func==default_wake_function);
 autoentry.func=autoremove_wake_function;
 wait_queue_head_t q={LIST_HEAD_INIT(q.head)};
 struct wait_queue_entry keep={0,NULL,persistent,LIST_HEAD_INIT(keep.entry)};
 struct wait_queue_entry skip={WQ_FLAG_EXCLUSIVE,NULL,ignored,LIST_HEAD_INIT(skip.entry)};
 autoentry.flags=WQ_FLAG_EXCLUSIVE;
 list_add(&autoentry.entry,&q.head);list_add(&skip.entry,&q.head);list_add(&keep.entry,&q.head);
 assert(__kpi_wake_up_common_locked(&q,1,0)==2);
 assert(woken_thread==&native && calls==1);
 assert(q.head.next==&keep.entry && keep.entry.next==&skip.entry);
 assert(autoentry.entry.next==&autoentry.entry);
 assert(__kpi_wake_up_common_locked(&q,1,0)==1 && calls==2);
 /* Stale jiffies must not charge 1000ms of pre-wait time to a 10ms wake. */
 jiffies=0;now=1000;elapsed=10;result=1;
 assert(schedule_timeout(100)==90);
 elapsed=100;result=0;assert(schedule_timeout(100)==0);
 assert(schedule_timeout(0)==0 && reset==1);
}
'''
with tempfile.TemporaryDirectory(prefix='kpi-wait-time-') as tmp:
 p=Path(tmp)/'check';p.with_suffix('.c').write_text(code)
 subprocess.run(['cc','-std=gnu11','-Wall','-Wextra','-Werror',str(p.with_suffix('.c')),'-o',str(p)],check=True)
 subprocess.run([str(p)],check=True,timeout=5)
print('PASS: task handle conversion, persistent/rejected/exclusive wakes, stale-jiffies timeout budget')
