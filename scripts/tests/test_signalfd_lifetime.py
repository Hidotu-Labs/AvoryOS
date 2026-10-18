#!/usr/bin/env python3
"""Race the actual signalfd notifier against descriptor close and exit detach."""
from pathlib import Path
import subprocess
import tempfile

source = (Path(__file__).resolve().parents[2] / 'kernel/src/syscalls/sys_signal.c').read_text()
start = source.index('static void signal_notify_signalfds(struct thread *t, int sig) {')
end = source.index('\nvoid signal_notify_thread', start)
code = r'''
#include <assert.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#define MAX_FDS 64
#define POLLIN 1
typedef pthread_mutex_t spinlock_t;
static void spinlock_acquire(spinlock_t *p){assert(!pthread_mutex_lock(p));}
static void spinlock_release(spinlock_t *p){assert(!pthread_mutex_unlock(p));}
spinlock_t tid_lock=PTHREAD_MUTEX_INITIALIZER;
typedef struct {uint64_t mask;int wq;} signalfd_ctx_t;
static void signalfd_read(void){}
typedef struct node { _Atomic int refs;void (*read)(void);void *device;} vfs_node_t;
struct fd_table {spinlock_t lock;vfs_node_t *fds[MAX_FDS];};
struct thread {struct fd_table *files;};
static bool fd_is_valid_node(vfs_node_t *n){return n&&n!=(void*)-1;}
static void vfs_node_ref(vfs_node_t *n){assert(atomic_fetch_add(&n->refs,1)>0);}
static void vfs_close(vfs_node_t *n){if(atomic_fetch_sub(&n->refs,1)==1){free(n->device);free(n);}}
static _Atomic unsigned wakes;
static void wait_queue_wake_all(int *wq){assert(*wq==123);atomic_fetch_add(&wakes,1);}
static void epoll_notify_event(vfs_node_t *n,int events){assert(atomic_load(&n->refs)>0);assert(events==POLLIN);}
'''
code += source[start:end]
code += r'''
static struct thread target;
static _Atomic bool done;
static void *notify(void *unused){(void)unused;while(!atomic_load(&done))signal_notify_signalfds(&target,17);return NULL;}
static struct fd_table *table(void){
 struct fd_table *f=calloc(1,sizeof(*f));assert(f);assert(!pthread_mutex_init(&f->lock,NULL));
 for(int i=0;i<MAX_FDS;i++){
  vfs_node_t *n=calloc(1,sizeof(*n));signalfd_ctx_t *c=malloc(sizeof(*c));assert(n&&c);
  c->mask=1ULL<<16;c->wq=123;n->refs=1;n->read=signalfd_read;n->device=c;f->fds[i]=n;
 }return f;
}
int main(void){
 target.files=table();signal_notify_signalfds(&target,17);assert(wakes==MAX_FDS);
 struct fd_table *f=target.files;target.files=NULL;
 for(int i=0;i<MAX_FDS;i++)vfs_close(f->fds[i]);pthread_mutex_destroy(&f->lock);free(f);
 signal_notify_signalfds(&target,17);
 pthread_t worker;assert(!pthread_create(&worker,NULL,notify,NULL));
 for(int round=0;round<5000;round++){
  f=table();spinlock_acquire(&tid_lock);target.files=f;spinlock_release(&tid_lock);
  for(int i=0;i<MAX_FDS;i+=2){
   spinlock_acquire(&f->lock);vfs_node_t *n=f->fds[i];f->fds[i]=(i%4)?NULL:(void*)-1;spinlock_release(&f->lock);vfs_close(n);
  }
  spinlock_acquire(&tid_lock);spinlock_acquire(&f->lock);target.files=NULL;spinlock_release(&f->lock);spinlock_release(&tid_lock);
  for(int i=0;i<MAX_FDS;i++)if(fd_is_valid_node(f->fds[i]))vfs_close(f->fds[i]);
  pthread_mutex_destroy(&f->lock);free(f);
 }
 atomic_store(&done,true);pthread_join(worker,NULL);
 puts("PASS: signalfd snapshots survive concurrent close, reserved slots and exit detach across batches");
}
'''
with tempfile.TemporaryDirectory(prefix='avory-signalfd-') as directory:
    path = Path(directory)
    (path / 'test.c').write_text(code)
    subprocess.run(['cc', '-std=gnu11', '-O1', '-g', '-fsanitize=address,undefined',
                    '-fno-omit-frame-pointer', '-pthread', path / 'test.c', '-o', path / 'test'], check=True)
    subprocess.run([path / 'test'], check=True)
