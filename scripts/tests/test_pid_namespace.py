#!/usr/bin/env python3
"""Exercise actual PID namespace mapping/refcounts with sanitizer instrumentation."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
source = (ROOT / 'kernel/src/sched/pid_namespace.c').read_text()
source = '\n'.join(line for line in source.splitlines() if not line.startswith('#include'))
fixture = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#define PID_NS_MAX_DEPTH 32
#define THREAD_DEAD 4
#define THREAD_ZOMBIE 5
typedef int spinlock_t;
struct pid_namespace;
struct thread {
 struct pid_namespace *pid_ns,*pid_children_ns;
 uint32_t namespace_pids[32],tid,tgid;
 bool is_main_session;
 spinlock_t namespace_lock;
 struct thread *global_next;
 int state;
 uint64_t pending_signals;
 uint32_t futex_wake_claims;
 struct {void *sa_handler;} signal_handlers[64];
};
spinlock_t tid_lock;
struct thread *global_thread_list;
static unsigned allocations,wakes;
static bool fail_alloc;
static void *kmalloc(size_t n) {if(fail_alloc)return NULL;void *p=malloc(n);if(p)allocations++;return p;}
static void kfree(void *p) {if(p)allocations--;free(p);}
static void spinlock_acquire(spinlock_t *p) {(void)p;}
static void spinlock_release(spinlock_t *p) {(void)p;}
static uint32_t namespace_allocate_id(void) {static uint32_t id=100;return ++id;}
static void sched_wakeup(struct thread *p) {assert(p->futex_wake_claims);wakes++;}
'''
checks = r'''
int main(void) {
 struct thread parent={.tid=10,.tgid=10}, init={.tid=20,.tgid=20},
   child={.tid=30,.tgid=30}, nested={.tid=40,.tgid=40}, bad={.tid=50,.tgid=50};
 parent.global_next=&init;init.global_next=&child;child.global_next=&nested;
 global_thread_list=&parent;
 fail_alloc=true;assert(!pid_namespace_create(&parent));fail_alloc=false;
 struct pid_namespace *ns=pid_namespace_create(&parent);assert(ns);
 assert(pid_namespace_inherit(&init,&parent,ns,false)==0);
 assert(pid_namespace_tid(&init,&init)==1 && pid_namespace_tid(&parent,&init)==20);
 assert(pid_namespace_tid(&init,&parent)==0);
 assert(pid_namespace_inherit(&child,&init,NULL,false)==0);
 assert(pid_namespace_tid(&init,&child)==2 && pid_namespace_tid(&child,&init)==1);
 struct pid_namespace *inner=pid_namespace_create(&init);assert(inner);
 assert(pid_namespace_inherit(&nested,&init,inner,false)==0);
 assert(pid_namespace_tid(&nested,&nested)==1 && pid_namespace_tid(&init,&nested)==3);
 assert(pid_namespace_global(&init,3)==40 && pid_namespace_visible(&nested,20)==0);
 uint32_t id=0;assert(pid_namespace_nth(&nested,0,&id) && id==40);
 assert(!pid_namespace_signal_allowed(&init,&init,15));
 assert(!pid_namespace_signal_allowed(&init,&init,9));
 assert(pid_namespace_signal_allowed(&parent,&init,9));
 init.signal_handlers[9].sa_handler=(void*)2;
 assert(pid_namespace_signal_allowed(&child,&init,10));
 pid_namespace_unshare(&init,inner); /* consumes creator reference */
 assert(pid_namespace_inherit(&bad,&init,NULL,true)==-22);
 pid_namespace_exit(&init);
 assert(wakes==2 && (child.pending_signals&256) && (nested.pending_signals&256));
 assert(pid_namespace_inherit(&bad,&child,NULL,false)==-12);
 pid_namespace_release(&nested);pid_namespace_release(&child);pid_namespace_release(&init);
 pid_namespace_put(ns);assert(allocations==0);
 return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='avory-pid-ns-') as directory:
    path=Path(directory);(path/'test.c').write_text(fixture+source+checks)
    subprocess.run(['cc','-std=gnu11','-Wall','-Wextra','-Werror','-g','-O1',
                    '-fsanitize=address,undefined',str(path/'test.c'),'-o',str(path/'test')],check=True)
    subprocess.run([str(path/'test')],check=True)
print('PID namespace sanitizer tests passed')
