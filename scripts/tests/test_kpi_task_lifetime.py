#!/usr/bin/env python3
"""Execute real task-shadow/kthread ownership paths with counted allocations."""
from pathlib import Path
import re, subprocess, tempfile
root = Path(__file__).resolve().parents[2]
def function(path, signature):
    text=(root/path).read_text(); start=text.index(signature)
    end=text.index('{',start)+1; depth=1
    while depth:
        depth+=(text[end]=='{')-(text[end]=='}');end+=1
    return text[start:end]+"\n"
def strip(text): return re.sub(r'^#include.*$', '', text, flags=re.M)
pre=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#define GFP_KERNEL 0
#define TASK_RUNNING 0
#define TASK_COMM_LEN 16
#define ENOMEM 12
#define EINVAL 22
#define WRITE_ONCE(x,v) ((x)=(v))
#define ERR_PTR(x) ((void*)(intptr_t)(x))
#define IS_ERR(x) ((uintptr_t)(x)>=(uintptr_t)-4095)
typedef int pid_t;
typedef unsigned refcount_t;
static void refcount_set(refcount_t *r,unsigned n){*r=n;}
static void refcount_inc(refcount_t *r){assert(*r);++*r;}
static bool refcount_dec_and_test(refcount_t *r){assert(*r);return !--*r;}
struct task_struct {void *kpi_thread;long state;pid_t pid,tgid;char comm[16];refcount_t usage;struct task_struct *group_leader;void *kpi_control;};
struct thread {void *kpi_task;};
static unsigned live, allocations, native_cancels;
static bool fail_shadow;
static void *kmalloc(size_t n,int g){(void)g;void *p=malloc(n);assert(p);++live;++allocations;return p;}
static void *kzalloc(size_t n,int g){if(fail_shadow)return NULL;void *p=kmalloc(n,g);memset(p,0,n);return p;}
static void kfree(void *p){if(p){assert(live);--live;free(p);}}
static unsigned long linuxkpi_thread_pid(void *t){(void)t;return 12;}
static unsigned long linuxkpi_thread_tgid(void *t){return linuxkpi_thread_pid(t);}
static void linuxkpi_thread_comm(void *t,char *p,unsigned long n){(void)t;snprintf(p,n,"mock");}
typedef struct {bool held;} spinlock_t;
static void spin_lock_init(spinlock_t *l){l->held=false;}
static void spin_lock(spinlock_t *l){assert(!l->held);l->held=true;}
static bool pending_start;
static void run_pending(void);
static void spin_unlock(spinlock_t *l){assert(l->held);l->held=false;if(pending_start)run_pending();}
struct completion {bool done;};
static void init_completion(struct completion *c){c->done=false;}
static void complete(struct completion *c){c->done=true;}
static void wait_for_completion(struct completion *c){assert(c->done);}
static struct thread native;
static void (*entry)(void*);
static void *entry_arg;
static unsigned wakes;
static void *linuxkpi_kthread_create(void(*fn)(void*),void *arg,const char *name){
 (void)name;assert(!native.kpi_task);entry=fn;entry_arg=arg;return &native;
}
static void *linuxkpi_current_thread(void){return &native;}
static void linuxkpi_kthread_cancel_create(void *t){assert(t==&native);native_cancels++;entry=NULL;entry_arg=NULL;}
static void linuxkpi_wake_thread(void *t){assert(t==&native);wakes++;pending_start=true;}
static void run_pending(void){pending_start=false;entry(entry_arg);}
void *linuxkpi_task_shadow_new(void *);
void linuxkpi_task_shadow_release(void *) __attribute__((weak));
static void *linuxkpi_task_for_thread(void *t){struct thread *n=t;n->kpi_task=linuxkpi_task_shadow_new(t);return n->kpi_task;}
struct task_struct *linuxkpi_task_get(struct task_struct *);
void linuxkpi_task_put(struct task_struct *);
void linuxkpi_thread_exiting(void *);
'''
code=pre+strip((root/'kernel/linuxkpi/src/task.c').read_text())
code+=function('kernel/src/linuxkpi/native_sched.c','void linuxkpi_thread_exiting(')
kt=(root/'kernel/linuxkpi/src/kthread.c').read_text()
a=kt.index('#define KPI_KTHREAD_MAGIC');b=kt.index('static void kpi_kthread_trampoline')
code+=kt[a:b]
for name in ['static void kpi_kthread_trampoline(', 'static struct kpi_kthread *kthread_control(', 'struct task_struct *kthread_create_on_node(', 'int kthread_stop(', 'void kthread_detach(']:
    code+=function('kernel/linuxkpi/src/kthread.c',name)
code+=r'''
static int fn(void *p){assert(p==(void*)42);return 0;}
int main(void){
 for(unsigned n=0;n<1000;n++){
  struct thread t={.kpi_task=linuxkpi_task_shadow_new(&t)};
  assert(live==1);linuxkpi_thread_exiting(&t);assert(!t.kpi_task && !live);
  linuxkpi_thread_exiting(&t);assert(!live);
 }
 // Imported code may keep a shadow after native exit.
 struct thread t={0};t.kpi_task=linuxkpi_task_shadow_new(&t);
 struct task_struct *held=linuxkpi_task_get(t.kpi_task);
 linuxkpi_thread_exiting(&t);assert(live==1 && !held->kpi_thread);
 linuxkpi_task_put(held);assert(!live);
 for(unsigned n=0;n<1000;n++){
  struct task_struct *task=kthread_create_on_node(fn,(void*)42,0,"test");
  assert(!IS_ERR(task) && task->usage==2 && live==2);
  // Stop before trampoline starts: stop must wake the suspended thread.
  assert(!kthread_stop(task));assert(task->usage==1 && live==2);
  linuxkpi_thread_exiting(&native);assert(!live);
  // Detach a self-retiring service: no join/stop needed to free it.
  task=kthread_create_on_node(fn,(void*)42,0,"service");
  kthread_detach(task);assert(task->usage==1);entry(entry_arg);
  linuxkpi_thread_exiting(&native);assert(!live);
  // Join after native thread has already exited/reaped.
  task=kthread_create_on_node(fn,(void*)42,0,"joined");
  entry(entry_arg);linuxkpi_thread_exiting(&native);
  assert(task->usage==1 && !task->kpi_thread && live==2);
  assert(!kthread_stop(task));assert(!live);
  // Temporary reference for destruction of a detached worker.
  task=kthread_create_on_node(fn,(void*)42,0,"worker");kthread_detach(task);
  linuxkpi_task_get(task);assert(!kthread_stop(task));
  linuxkpi_thread_exiting(&native);assert(!live);
 }
 fail_shadow=true;
 assert(IS_ERR(kthread_create_on_node(fn,(void*)42,0,"failed")));
 assert(native_cancels==1 && !live);
 printf("PASS: counted task/control ownership; 1000 exits and 4000 stop/detach lifecycles, OOM cancellation, %u wakes, %u allocations, zero live allocations\n",wakes,allocations);
}
'''
with tempfile.TemporaryDirectory(prefix='kpi-task-lifetime-') as tmp:
    c=Path(tmp)/'test.c';exe=Path(tmp)/'test';c.write_text(code)
    subprocess.run(['cc','-std=gnu11','-O1','-g','-Wall','-Wextra','-Werror','-Wno-address','-fsanitize=address,undefined','-fno-omit-frame-pointer',str(c),'-o',str(exe)],check=True)
    import os
    env=dict(os.environ,ASAN_OPTIONS='detect_leaks=0')
    subprocess.run([str(exe)],env=env,check=True,timeout=30)
    old = code.replace('  kfree(tsk->kpi_control);\n  kfree(tsk);', '  (void)tsk;')
    assert old != code
    c.write_text(old)
    subprocess.run(['cc','-std=gnu11','-O1','-Wno-address',str(c),'-o',str(exe)],check=True)
    failed = subprocess.run([str(exe)],capture_output=True,timeout=20)
    assert failed.returncode != 0, 'Original no-op final task put was not detected'
    print('PASS: original no-op final task put fails the counted-allocation regression')
