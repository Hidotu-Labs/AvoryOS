#!/usr/bin/env python3
"""Exercise the kernel's queued-signal dispatcher used by real udev workers."""
from pathlib import Path
import subprocess
import tempfile

source = (Path(__file__).resolve().parents[2] / 'kernel/src/syscalls/sys_signal.c').read_text()
structures = source[source.index('struct kernel_siginfo {'):source.index('#define MAX_QUEUED_SIGINFO')]
dispatch = source[source.index('static uint64_t queue_siginfo('):source.index('#define MINSIGSTKSZ')]
fixture = r'''
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <stdio.h>
#define THREAD_DEAD 3
#define THREAD_ZOMBIE 4
#define SIGCONT 18
#define SIG_IGN 1
#define MAX_QUEUED_SIGINFO 64
#define CAP_KILL 5
struct queued_siginfo;
struct thread {
 uint32_t tid,tgid,uid,euid,suid,sid,pid_offset;
 int state;
 uint64_t signal_mask,pending_signals,thread_directed_signals;
 struct { void *sa_handler; } signal_handlers[64];
 uint32_t signal_sender_pid[64];int signal_sender_code[64];
 struct queued_siginfo *queued_siginfo;
 struct thread *global_next;
};
typedef int spinlock_t;
spinlock_t tid_lock;
struct thread *global_thread_list,*sender,*notified;
static void *kmalloc(size_t n){return malloc(n);}
static void kfree(void *p){free(p);}
static int copy_from_user(void *d,const void *s,size_t n){if(!s)return 1;memcpy(d,s,n);return 0;}
static struct thread *sched_get_current(void){return sender;}
static void spinlock_acquire(int *l){assert(!*l);*l=1;}
static void spinlock_release(int *l){assert(*l);*l=0;}
static void signal_notify_thread(struct thread *t,int sig){assert(!tid_lock&&sig);notified=t;}
static uint32_t pid_namespace_tid(struct thread *v,struct thread *t){return t->tid>v->pid_offset?t->tid-v->pid_offset:0;}
static uint32_t pid_namespace_visible_locked(struct thread *v,uint32_t pid){return pid>v->pid_offset?pid-v->pid_offset:0;}
static bool capability_has(struct thread *t,unsigned cap){assert(cap==CAP_KILL);return t->euid==0;}
static bool pid_namespace_signal_allowed(struct thread *s,struct thread *t,int sig){(void)s;(void)t;(void)sig;return true;}

'''
checks = r'''
int main(void){
 struct thread caller={.tid=10,.tgid=10},leader={.tid=20,.tgid=20,.uid=100},worker={.tid=21,.tgid=20,.uid=100};
 caller.global_next=&leader;leader.global_next=&worker;
 global_thread_list=&caller;sender=&caller;
 unsigned sig=40;uint64_t bit=1ULL<<(sig-1);
 struct kernel_siginfo info={.si_code=-1};
 info._kill.si_pid=10;info._kill.si_uid=0;
 uint64_t payload=0x123456789abcdef0ULL;memcpy(info.data+8,&payload,8);
 leader.signal_mask=bit;
 assert(!sys_rt_sigqueueinfo(20,sig,(uintptr_t)&info,0,0,0));
 assert(notified==&worker && worker.pending_signals==bit && !worker.thread_directed_signals);
 assert(!memcmp(worker.queued_siginfo->info.data+8,&payload,8));
 assert(!sys_rt_tgsigqueueinfo(20,20,sig,(uintptr_t)&info,0,0));
 assert(notified==&leader && leader.thread_directed_signals==bit);
 assert((int64_t)sys_rt_sigqueueinfo(20,0,0,0,0,0)==-14);
 info.si_code=0;
 assert((int64_t)sys_rt_sigqueueinfo(10,sig,(uintptr_t)&info,0,0,0)==-1);
 assert((int64_t)sys_rt_tgsigqueueinfo(20,20,sig,(uintptr_t)&info,0,0)==-1);
 info.si_code=-1;caller.uid=caller.euid=200;
 assert((int64_t)sys_rt_sigqueueinfo(20,sig,(uintptr_t)&info,0,0,0)==-1);
 caller.euid=0;leader.state=THREAD_ZOMBIE;worker.signal_mask=bit;
 assert(!sys_rt_sigqueueinfo(20,sig,(uintptr_t)&info,0,0,0));
 assert(notified==&worker);
 assert((int64_t)sys_rt_sigqueueinfo(999,sig,(uintptr_t)&info,0,0,0)==-3);
 caller.tid=caller.tgid=19;caller.pid_offset=18;
 assert((int64_t)sys_rt_sigqueueinfo(20,sig,(uintptr_t)&info,0,0,0)==-3);
 assert(!sys_rt_sigqueueinfo(2,sig,(uintptr_t)&info,0,0,0));
 assert(notified==&worker);
 assert(!sys_rt_tgsigqueueinfo(2,3,sig,(uintptr_t)&info,0,0));
 for(struct thread *t=&caller;t;t=t->global_next){
  while(t->queued_siginfo){struct queued_siginfo *q=t->queued_siginfo;t->queued_siginfo=q->next;free(q);}
 }
 puts("PASS: queued payload, process/thread recipients, blocked/dead leaders, permissions and invalid input");
}
'''
with tempfile.TemporaryDirectory(prefix='avory-sigqueue-') as temp:
    path = Path(temp)
    (path / 'test.c').write_text(fixture + structures + dispatch + checks)
    subprocess.run(['cc', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
                    str(path / 'test.c'), '-o', str(path / 'test')], check=True)
    subprocess.run([str(path / 'test')], check=True)
