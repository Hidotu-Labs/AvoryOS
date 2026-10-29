#!/usr/bin/env python3
"""Run the actual epoll pool/FD lifecycle code in a host harness."""
from pathlib import Path
import os
import re
import subprocess
import tempfile


def block(text, signature):
    start = text.index(signature)
    end = text.index('{', start) + 1
    depth = 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[start:end]


epoll = Path('kernel/src/socket/epoll.c').read_text()
vfs = Path('kernel/src/fs/vfs.c').read_text()
syscalls = Path('kernel/src/syscalls/sys_epoll.c').read_text()
header = Path('kernel/src/socket/epoll.h').read_text()
code = r'''
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
typedef struct { bool held; } spinlock_t;
#define SPINLOCK_INIT {false}
static void spinlock_init(spinlock_t *s){s->held=false;}
static void spinlock_acquire(spinlock_t *s){assert(!s->held);s->held=true;}
static void spinlock_release(spinlock_t *s){assert(s->held);s->held=false;}
struct list_head {struct list_head *next,*prev;};
#define INIT_LIST_HEAD(h) ((h)->next=(h),(h)->prev=(h))
static void list_del(struct list_head *h){if(!h||!h->next||!h->prev)return;h->prev->next=h->next;h->next->prev=h->prev;h->next=h->prev=NULL;}
static bool list_empty(struct list_head *h){return h->next==h;}
static void list_add_tail(struct list_head *e,struct list_head *h){e->next=h;e->prev=h->prev;h->prev->next=e;h->prev=e;}
#define list_entry(p,t,m) ((t*)((char*)(p)-__builtin_offsetof(t,m)))
#define list_first_entry(p,t,m) list_entry((p)->next,t,m)
typedef struct {int dummy;void *head;} wait_queue_t;
static void wait_queue_init(wait_queue_t *w){w->dummy=0;w->head=NULL;}
static void wait_queue_wake_all(void *w){(void)w;}
static void wait_queue_wake_one(void *w){(void)w;}
#define klog_puts(...) ((void)0)
#define klog_uint64(...) ((void)0)
#define FS_EPOLL 8
#define FS_TYPE_MASK 15
#define FS_PIPE 3
#define FS_PERSISTENT 256
#define FS_DELETED 512
#define FS_AUTODESTROY 1024
#define MAX_FDS 16
'''
code += re.sub(r'^#include .*\n', '', header, flags=re.M)
code += r'''
static eventpoll_t *epoll_table[EPOLL_MAX_INSTANCES];
#define EPOLL_BITMAP_WORDS ((EPOLL_MAX_INSTANCES + 63) / 64)
static uint64_t epoll_free_bitmap[EPOLL_BITMAP_WORDS];
static spinlock_t epoll_table_lock = SPINLOCK_INIT;
typedef struct vfs_node {
 struct vfs_node *view_backing;
 char name[64];uint32_t flags,inode,refcount,magic,current_events;
 void *device,*wait_queue;
 struct list_head ep_watchers;spinlock_t ep_lock;
 uint32_t (*read)(struct vfs_node *,uint32_t,uint32_t,uint8_t *);
 uint32_t (*write)(struct vfs_node *,uint32_t,uint32_t,uint8_t *);
 void (*open)(struct vfs_node *),(*close)(struct vfs_node *),(*destroy)(struct vfs_node *);
 int (*poll)(struct vfs_node *,int);
} vfs_node_t;
struct fd_table {spinlock_t lock;uint32_t next_fd,capacity;};
struct thread {int state;uint64_t wakeup_ticks;struct vfs_node *active_epoll_node;struct epitem *active_epoll_items[64];struct fd_table *files;vfs_node_t *fds[MAX_FDS];uint64_t fd_offsets[MAX_FDS],fd_flags[MAX_FDS];};
static struct fd_table files={.capacity=MAX_FDS};
static struct thread task={.files=&files};
static bool fail_alloc,reenter_close;
static int allocations,replacement_fd;
static vfs_node_t *closing_node;
static struct thread *running=&task;
static struct thread *sched_get_current(void){return running;}
#define THREAD_RUNNING 0
#define THREAD_BLOCKED 2
#define EPOLL_RESCAN_INTERVAL_MS 1000
struct wait_queue_entry {struct thread *thread;struct wait_queue_entry *next;};
typedef struct wait_queue_entry wait_queue_entry_t;
static void wait_queue_add(wait_queue_t *w,wait_queue_entry_t *e){(void)w;(void)e;}
static void wait_queue_remove(wait_queue_t *w,wait_queue_entry_t *e){(void)w;(void)e;}
static bool thread_has_pending_signal(struct thread *t){(void)t;return false;}
static uint64_t lapic_timer_get_ticks(void){return 100;}
static void sched_yield(void){assert(!"unexpected blocking in test");}
static bool fail_copy;
static unsigned long copy_to_user(void *d,const void *s,unsigned long n){if(fail_copy)return n;memcpy(d,s,n);return 0;}
static void (*poll_hook)(void);
static int vfs_poll(vfs_node_t *n,int events){int ready=n->current_events & events;if(poll_hook){void(*h)(void)=poll_hook;poll_hook=NULL;h();}return ready;}
void epoll_notify_event(struct vfs_node *n,uint32_t e){(void)n;(void)e;}
static void *kmalloc(size_t n){if(fail_alloc)return NULL;void *p=malloc(n);assert(p);allocations++;return p;}
static void kfree(void *p){if(p){allocations--;free(p);}}
static void vfs_node_init(vfs_node_t *n){memset(n,0,sizeof(*n));n->refcount=1;n->magic=123;INIT_LIST_HEAD(&n->ep_watchers);}
static void vfs_node_ref(vfs_node_t *n){assert(n->refcount);n->refcount++;}
static bool vfs_node_unref(vfs_node_t *n){assert(n->refcount);return --n->refcount==0;}
static int alloc_fd(struct thread *t){for(int i=0;i<MAX_FDS;i++)if(!t->fds[i]){t->fds[i]=(void*)-1;return i;}return -1;}
static void fd_release_reserved(struct thread *t,int fd){assert(t->fds[fd]==(void*)-1);t->fds[fd]=NULL;}
static void fd_path_set(struct thread *t,int fd,const char *s){assert(t->files->lock.held);assert(t->fds[fd]!=(void*)-1);(void)s;}
static void fd_path_clear(struct thread *t,int fd){(void)t;(void)fd;}
static void vfs_cache_sync(vfs_node_t *n);
static bool vfs_node_claim_reclaim(vfs_node_t *n){(void)n;return true;}
static void vfs_node_release_parent(vfs_node_t *n){(void)n;}
static void vfs_node_reclaim_done(vfs_node_t *n,bool released){(void)n;(void)released;}
static void vfs_cache_clear(vfs_node_t *n){(void)n;}
static void vfs_xattr_clear(vfs_node_t *n){(void)n;}
static void vfs_rcu_defer_free(vfs_node_t *n){kfree(n);}
static void vfs_close(vfs_node_t *n);
uint32_t epoll_vfs_read(struct vfs_node *n,uint32_t o,uint32_t s,uint8_t *b){(void)n;(void)o;(void)s;(void)b;return 0;}
uint32_t epoll_vfs_write(struct vfs_node *n,uint32_t o,uint32_t s,uint8_t *b){(void)n;(void)o;(void)s;(void)b;return 0;}
int epoll_vfs_poll(struct vfs_node *n,int e){(void)n;(void)e;return 0;}
'''
for sig in ('static int epoll_table_alloc(', 'static void epoll_table_free(',
            'static epitem_t *epitem_alloc(', 'static void epitem_free(', 'static void epitem_pin(', 'static void epitem_unpin(',
            'void epoll_release_thread(', 'eventpoll_t *epoll_create(',
            'static bool epoll_expand_items_locked(',
            'void epoll_destroy(', 'void epoll_get(', 'void epoll_put(',
            'static inline uint32_t ep_check_events_mask(', 'static uint32_t ep_check_events(', 'static void ep_add_to_ready_list(',
            'int epoll_ctl_del(', 'int epoll_ctl_add(', 'int epoll_ctl_mod(',
            'int epoll_wait_impl(', 'int epoll_alloc_fd(', 'eventpoll_t *epoll_from_fd(', 'int epoll_close_fd(',
            'void epoll_vfs_close(', 'void epoll_init('):
    code += block(epoll, sig) + '\n'
code += block(vfs, 'void vfs_open(') + '\n'
code += block(vfs, 'void vfs_close(') + '\n'
code += block(syscalls, 'static uint64_t sys_epoll_create(') + '\n'
code += block(syscalls, 'static uint64_t sys_epoll_create1(') + '\n'
code += r'''
static eventpoll_t *racing_ep;
static int racing_fd;
static void notify_during_poll(void){
 epitem_t *item=racing_ep->items[racing_fd];assert(item && item->in_scan);
 vfs_node_t *node=item->node;node->current_events=EPOLLIN;
 __atomic_fetch_or(&item->ready_events,EPOLLIN,__ATOMIC_RELAXED);
 ep_add_to_ready_list(racing_ep,item);
}
static void remove_during_poll(void){
 struct thread peer={.files=&files};
 running=&peer;
 assert(epoll_ctl_del(racing_ep,racing_fd)==0);
 running=&task;
 // The removed item is still pinned by the waiting thread; a late
 // readiness callback must not enqueue it again.
 epitem_t *old=task.active_epoll_items[0];assert(old && old->removed);
 ep_add_to_ready_list(racing_ep,old);
 assert(racing_ep->rdllist_count==0);
}
static void vfs_cache_sync(vfs_node_t *n){
 if(reenter_close){
  assert(n==closing_node);reenter_close=false;
  eventpoll_t *ep=epoll_create();assert(ep);
  replacement_fd=epoll_alloc_fd(ep,EPOLL_CLOEXEC);assert(replacement_fd>=0);
  assert(task.fds[replacement_fd]!=n);
 }
}
int main(void){
 epoll_init();
 // Exercise every bitmap word and exact exhaustion, then recycle all slots.
 eventpoll_t *all[EPOLL_MAX_INSTANCES];
 for(int i=0;i<EPOLL_MAX_INSTANCES;i++){all[i]=epoll_create();assert(all[i]);}
 assert(!epoll_create());
 for(int i=0;i<EPOLL_MAX_INSTANCES;i++)epoll_put(all[i]);
 assert((int64_t)sys_epoll_create(0,0,0,0,0,0)==-22);
 assert((int64_t)sys_epoll_create1(1,0,0,0,0,0)==-22);
 // Repeated dup/fork-style VFS opens must not leak the instance ref.
 for(int i=0;i<2000;i++){
  int fd=(int)sys_epoll_create1(EPOLL_CLOEXEC,0,0,0,0,0);assert(fd>=0);
  vfs_node_t *n=task.fds[fd];eventpoll_t *ep=n->device;
  assert(task.fd_flags[fd]==(1ULL<<24));
  vfs_open(n);vfs_open(n);assert(ep->refcount==1);
  assert(epoll_close_fd(fd)==0);assert(n->refcount==2);
  vfs_close(n);assert(n->refcount==1);vfs_close(n);
 }
 // Node-allocation failure releases the fd reservation and pool slot.
 fail_alloc=true;
 assert((int64_t)sys_epoll_create1(0,0,0,0,0,0)==-12);
 fail_alloc=false;
 for(int i=0;i<MAX_FDS;i++)assert(!task.fds[i]);
 // All fds full returns EMFILE and rolls back the epoll slot.
 for(int i=0;i<MAX_FDS;i++)task.fds[i]=(void*)-1;
 assert((int64_t)sys_epoll_create1(0,0,0,0,0,0)==-24);
 memset(task.fds,0,sizeof(task.fds));
 // Simulate slot reuse before vfs_close finishes cleaning up the old node.
 int fd=(int)sys_epoll_create1(0,0,0,0,0,0);assert(fd>=0);
 closing_node=task.fds[fd];reenter_close=true;
 assert(epoll_close_fd(fd)==0);
 vfs_node_t *replacement=task.fds[replacement_fd];
 assert(replacement->ep_watchers.next==&replacement->ep_watchers);
 assert(replacement->device && replacement->wait_queue);
 assert(epoll_close_fd(replacement_fd)==0);
 for(int i=0;i<EPOLL_MAX_INSTANCES;i++){all[i]=epoll_create();assert(all[i]);}
 assert(!epoll_create());
 for(int i=0;i<EPOLL_MAX_INSTANCES;i++)epoll_put(all[i]);
 // Descriptor close during an active syscall cannot recycle the instance.
 fd=(int)sys_epoll_create1(0,0,0,0,0,0);assert(fd>=0);
 eventpoll_t *ep=epoll_from_fd(fd);assert(ep);
 assert(epoll_close_fd(fd)==0);assert(ep->refcount==1);
 epoll_release_thread(&task);
 // Reproduce DEL during the unlocked readiness poll, then late requeue.
 fd=(int)sys_epoll_create1(0,0,0,0,0,0);assert(fd>=0);
 ep=epoll_from_fd(fd);
 vfs_node_t *watched=kmalloc(sizeof(*watched));vfs_node_init(watched);
 watched->current_events=EPOLLIN;
 task.fds[1]=watched;
 struct epoll_event interest={.events=EPOLLIN,.data.u64=42}, out[2];
 assert(epoll_ctl_add(ep,1,&interest)==0);
 assert(ep->rdllist_count==1);
 racing_ep=ep;racing_fd=1;poll_hook=remove_during_poll;
 assert(epoll_wait_impl(ep,out,2,0)==0);
 assert(ep->item_count==0 && ep->rdllist_count==0);
 // A notification delivered after poll's sample but before reconciliation
 // must keep the item queued for a fresh readiness check.
 assert(epoll_ctl_add(ep,1,&interest)==0);
 watched->current_events=0;
 racing_ep=ep;racing_fd=1;poll_hook=notify_during_poll;
 assert(epoll_wait_impl(ep,out,2,0)==0);
 assert(ep->rdllist_count==1 && ep->items[1]->on_ready_list && !ep->items[1]->in_scan);
 assert(epoll_wait_impl(ep,out,2,0)==1 && out[0].data.u64==42);
 watched->current_events=EPOLLIN;
 // Repeated LT delivery keeps the links sound and drops all temporary pins.
 for(int i=0;i<100;i++){
  assert(epoll_wait_impl(ep,out,2,0)==1 && out[0].data.u64==42);
  assert(ep->rdllist_count==1 && !task.active_epoll_items[0]);
 }
 // Bad output pointers return EFAULT, not a kernel fault or pinned leak.
 fail_copy=true;assert(epoll_wait_impl(ep,out,2,0)==-14);fail_copy=false;
 assert(!task.active_epoll_items[0]);
 assert(epoll_ctl_mod(ep,1,&interest)==0);
 // Forced-exit cleanup also releases a detached polling snapshot.
 spinlock_acquire(&ep->lock);
 epitem_t *pending=list_first_entry(&ep->rdllist,epitem_t,rdllink);
 epitem_pin(pending,0);list_del(&pending->rdllink);
 pending->on_ready_list=false;ep->rdllist_count--;
 spinlock_release(&ep->lock);
 assert(epoll_close_fd(fd)==0);
 epoll_release_thread(&task);
 task.fds[1]=NULL;vfs_close(watched);
 assert(allocations==0);
 puts("epoll lifecycle: capacity, reuse, dup/fork refs, rollback, CLOEXEC, close callback reuse, DEL and missed-notification poll races, active-close, forced-exit cleanup passed");
}
'''
with tempfile.TemporaryDirectory(prefix='avory-epoll-') as d:
    src = Path(d) / 'test.c'
    exe = Path(d) / 'test'
    src.write_text(code)
    subprocess.run(['cc', '-std=gnu11', '-O1', '-g', '-fsanitize=address,undefined',
                    '-Wall', '-Wextra', str(src), '-o', str(exe)], check=True)
    # LeakSanitizer can fail under constrained runners even when the harness's
    # explicit allocation accounting passes; keep ASan/UBSan instrumentation.
    env = dict(os.environ)
    env.setdefault('ASAN_OPTIONS', 'detect_leaks=0')
    subprocess.run([str(exe)], check=True, env=env)
