#!/usr/bin/env python3
"""Stress the actual LinuxKPI mutex fast/slow paths with competing threads."""
from pathlib import Path
import re
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'kernel/linuxkpi/src/mutex.c').read_text()
source = re.sub(r'^#include.*\n', '', source, flags=re.MULTILINE)
code = r'''
#define _GNU_SOURCE
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdatomic.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#define ERESTARTSYS 512
struct list_head { struct list_head *next,*prev; };
static void INIT_LIST_HEAD(struct list_head *l) { l->next=l->prev=l; }
static bool list_empty(struct list_head *l) { return l->next==l; }
static void list_add_tail(struct list_head *l,struct list_head *h) {
 l->prev=h->prev;l->next=h;h->prev->next=l;h->prev=l;
}
static void list_del_init(struct list_head *l) {
 l->next->prev=l->prev;l->prev->next=l->next;INIT_LIST_HEAD(l);
}
#define list_first_entry(head,type,member) ((type*)((char*)(head)->next - offsetof(type,member)))
struct task_struct { pthread_mutex_t lock;pthread_cond_t cond;bool pending; };
static _Thread_local struct task_struct task;
#define current (&task)
struct mutex { struct task_struct *owner;pthread_mutex_t wait_lock;struct list_head wait_list; };
static void spin_lock_init(pthread_mutex_t *l) { pthread_mutex_init(l,NULL); }
static void spin_lock(pthread_mutex_t *l) { pthread_mutex_lock(l); }
static void spin_unlock(pthread_mutex_t *l) { pthread_mutex_unlock(l); }
static bool signal_pending(struct task_struct *t) { (void)t;return false; }
static void *task_struct_to_thread(struct task_struct *t) { return t; }
static void linuxkpi_mutex_wait_set(void *p) { (void)p; }
static void linuxkpi_thread_block(void) {
 pthread_mutex_lock(&task.lock);
 while(!task.pending)pthread_cond_wait(&task.cond,&task.lock);
 task.pending=false;pthread_mutex_unlock(&task.lock);
}
static void linuxkpi_wake_thread(void *p) {
 struct task_struct *t=p;pthread_mutex_lock(&t->lock);t->pending=true;
 pthread_cond_signal(&t->cond);pthread_mutex_unlock(&t->lock);
}
'''
code += source
code += r'''
static struct mutex lock;
static pthread_barrier_t barrier;
static atomic_int inside;
static unsigned count;
static void *worker(void *p) {
 (void)p;pthread_mutex_init(&task.lock,NULL);pthread_cond_init(&task.cond,NULL);
 for(int i=0;i<12000;i++) {
  if(i%16==0)pthread_barrier_wait(&barrier);
  mutex_lock(&lock);
  assert(atomic_fetch_add(&inside,1)==0);
  count++;
  if(i%7==0)sched_yield();
  assert(atomic_fetch_sub(&inside,1)==1);
  mutex_unlock(&lock);
 }
 return NULL;
}
int main(void) {
 pthread_t t[8];mutex_init(&lock);pthread_barrier_init(&barrier,NULL,8);
 for(int i=0;i<8;i++)assert(!pthread_create(&t[i],NULL,worker,NULL));
 for(int i=0;i<8;i++)pthread_join(t[i],NULL);
 assert(count==96000 && !mutex_is_locked(&lock) && list_empty(&lock.wait_list));
 puts("PASS: 96000 contended acquisitions preserve single ownership and drain every waiter");
}
'''
with tempfile.TemporaryDirectory(prefix='avory-kpi-mutex-') as tmp:
    p = Path(tmp)
    (p / 'check.c').write_text(code)
    subprocess.run(['cc', '-O2', '-pthread', '-std=c11', '-Wall', '-Wextra',
                    '-Werror', str(p / 'check.c'), '-o', str(p / 'check')], check=True)
    subprocess.run([str(p / 'check')], check=True, timeout=20)
