#!/usr/bin/env python3
"""Actual-source poll registration and directory-alias rename regressions."""
from pathlib import Path
import os, subprocess, tempfile

def extract(path, signature):
 s=Path(path).read_text(); a=s.index(signature); b=s.index('{',a); depth=1; end=b+1
 while depth:
  depth+=(s[end]=='{')-(s[end]=='}');end+=1
 return s[a:end]
prefix=r'''
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#define MAX_FDS 4
#define THREAD_RUNNING 1
#define THREAD_BLOCKED 2
#define POLLNVAL 32
#define POLLERR 8
#define POLLHUP 16
#define POLL_SAFETY_NET_MS 1000
struct vfs_node;
typedef struct vfs_node vfs_node_t;
struct vfs_node {char *name; void *wait_queue; void *device; unsigned inode; int (*rename)(vfs_node_t *,char *,char *);};
typedef struct {int linked;} wait_queue_t;
typedef struct {void *thread,*next,*wq,*thread_next,*thread_prev;} wait_queue_entry_t;
struct fd_path {char *value;};
struct thread {int state; uint64_t wakeup_ticks; vfs_node_t **fds; struct fd_path **fd_paths;
uint64_t last_poll_nfds; int last_poll_fds[8]; short last_poll_events[8]; char last_poll_paths[8][48];};
struct pollfd {int fd;short events,revents;};
static struct thread self;
static uint64_t now;
static int yields, mode, ready;
static bool irq_enabled=true;
typedef bool hal_irq_state_t;
static hal_irq_state_t hal_irq_save(void){bool old=irq_enabled;irq_enabled=false;return old;}
static void hal_irq_restore(hal_irq_state_t f){assert(self.state!=THREAD_BLOCKED || self.wakeup_ticks);irq_enabled=f;}
static struct thread *sched_get_current(void){return &self;}
static bool thread_has_pending_signal(struct thread *t){(void)t;return mode==3 && yields>0;}
static uint64_t lapic_timer_get_ticks(void){return now;}
static bool fd_is_valid_node(vfs_node_t *n){return n!=NULL;}
static int vfs_poll(vfs_node_t *n,int ev){(void)n;(void)ev;return ready;}
static void *kmalloc(size_t n){return malloc(n);}
static void kfree(void *p){free(p);}
static void wait_queue_add(wait_queue_t *q,wait_queue_entry_t *e){
 assert(!irq_enabled && self.state==THREAD_BLOCKED && self.wakeup_ticks);q->linked++;e->wq=q;
 if(mode==1){ready=1;self.state=THREAD_RUNNING;self.wakeup_ticks=0;}
}
static void wait_queue_remove(wait_queue_t *q,wait_queue_entry_t *e){q->linked--;e->wq=NULL;}
static void sched_yield(void){assert(irq_enabled);yields++;now=self.wakeup_ticks;self.state=THREAD_RUNNING;if(mode==2)ready=1;}
static int rename_calls;
static int invalidations,pins;
int ext2_rename_impl(vfs_node_t *p,char *a,char *b){(void)p;(void)a;(void)b;rename_calls++;return 0;}
int vfs_rename(vfs_node_t *p,char *a,char *b){return p->rename(p,a,b);}
void vfs_dentry_invalidate(vfs_node_t *p,char *n){(void)p;(void)n;invalidations++;}
int tmpfs_rename_cross(vfs_node_t *p,char *a,vfs_node_t *q,char *b){(void)p;(void)q;(void)a;(void)b;return -18;}
vfs_node_t *vfs_finddir(vfs_node_t *p,char *n){(void)p;(void)n;assert(!invalidations);pins++;return NULL;}
void vfs_node_retire(vfs_node_t *p){(void)p;}
void vfs_close(vfs_node_t *p){(void)p;}
static int cross_calls;
int ext2_rename_cross(vfs_node_t *p,char *a,vfs_node_t *q,char *b){(void)p;(void)q;(void)a;(void)b;cross_calls++;return 0;}
'''
main=r'''
int main(void){
 wait_queue_t q={0};vfs_node_t node={.name="test",.wait_queue=&q};vfs_node_t *fds[4]={&node};self.fds=fds;
 struct pollfd fd={.fd=0,.events=1};
 ready=1|64;assert(poll_check_fds(&fd,1,&self)==1 && fd.revents==1);
 ready=4|256;assert(poll_check_fds(&fd,1,&self)==0 && !fd.revents);
 ready=8192;assert(poll_check_fds(&fd,1,&self)==0);
 fd.events|=8192;assert(poll_check_fds(&fd,1,&self)==1 && fd.revents==8192);
 fd.events=1;ready=8|16;assert(poll_check_fds(&fd,1,&self)==1 && fd.revents==(8|16));
 ready=0;
 mode=1;assert(do_poll(&fd,1,100)==1 && !yields && !q.linked);
 mode=2;ready=0;assert(do_poll(&fd,1,100)==1 && yields==1 && !q.linked);
 mode=0;ready=0;yields=0;uint64_t start=now;
 assert(do_poll(&fd,1,20)==0 && now==start+20 && yields==1 && !q.linked);
 mode=0;yields=0;start=now;
 assert(do_poll(NULL,0,20)==0 && now==start+20 && yields==1);
 mode=3;yields=0;assert((int64_t)do_poll(NULL,0,UINT64_MAX)==-4 && yields==1);
 mode=0;
 puts("PASS: zero-fd finite and infinite polls honor timeout and signals");
 puts("PASS: poll registration masks interrupts, catches raced readiness, wakes and times out");
 int mount1,mount2;vfs_node_t a={.device=&mount1,.inode=42,.rename=ext2_rename_impl};vfs_node_t b=a;
 assert(vfs_rename_cross(&a,"old",&b,"new")==0 && rename_calls==1);
 b.device=&mount2;assert(vfs_rename_cross(&a,"old",&b,"new")==-18 && rename_calls==1);
 invalidations=pins=0;b.device=&mount1;b.inode=43;assert(vfs_rename_cross(&a,"old",&b,"new")==0 && rename_calls==1 && cross_calls==1 && pins==2 && invalidations==4);
 puts("PASS: ext2 aliases and same-mount directory moves route correctly; cross-mount moves return EXDEV");
}
'''
code=prefix+extract(os.environ.get('POLL_SOURCE','kernel/src/syscalls/sys_poll.c'),'static int poll_check_fds(')+extract(os.environ.get('POLL_SOURCE','kernel/src/syscalls/sys_poll.c'),'static uint64_t do_poll(')+extract(os.environ.get('VFS_SOURCE','kernel/src/fs/vfs.c'),'int vfs_rename_cross(')+main
with tempfile.TemporaryDirectory() as tmp:
 p=Path(tmp);(p/'test.c').write_text(code)
 subprocess.run(['cc','-O1','-g','-fsanitize=address,undefined',str(p/'test.c'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True,env={**os.environ,'ASAN_OPTIONS':'detect_leaks=0'})
