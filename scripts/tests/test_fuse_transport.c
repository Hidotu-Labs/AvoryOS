/* Actual FUSE device/queue code under ASan/UBSan; no userspace daemon mocks
 * replace the framing, request ownership, cancellation or disconnect logic. */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define LOCK_SPINLOCK_H
#define ARCH_UACCESS_H
#define sched_yield fuse_test_sched_yield
typedef struct {unsigned locked;uint64_t flags;} spinlock_t;
typedef struct {unsigned locked;} rawspinlock_t;
#define SPINLOCK_INIT {0}
#define RAWSPINLOCK_INIT {0}
static void spinlock_init(spinlock_t *p){memset(p,0,sizeof(*p));}
static void spinlock_acquire(spinlock_t *p){assert(!p->locked);p->locked=1;}
static void spinlock_release(spinlock_t *p){assert(p->locked);p->locked=0;}
static bool user,copy_fault,alloc_fault;
static bool is_user_ptr(uint64_t p){(void)p;return user;}
static unsigned long copy_from_user(void *d,const void *s,unsigned long n){if(copy_fault)return n;memcpy(d,s,n);return 0;}
static unsigned long copy_to_user(void *d,const void *s,unsigned long n){if(copy_fault)return n;memcpy(d,s,n);return 0;}
#include "../../kernel/src/fs/fuse.c"
#undef sched_yield
static struct thread task;
static unsigned allocations;
void *kmalloc(size_t n){if(alloc_fault)return NULL;void *p=malloc(n?n:1);if(p)allocations++;return p;}
void *kcalloc(size_t n,size_t s){void *p=kmalloc(n*s);if(p)memset(p,0,n*s);return p;}
void kfree(void *p){if(p){assert(allocations);allocations--;free(p);}}
struct thread *sched_get_current(void){return &task;}
void fuse_test_sched_yield(void){assert(0&&"test must not block");}
void wait_queue_init(wait_queue_t *q){memset(q,0,sizeof(*q));}
void wait_queue_wake_all(wait_queue_t *q){(void)q;}
void wait_queue_add(wait_queue_t *q,wait_queue_entry_t *e){(void)q;(void)e;}
void wait_queue_remove(wait_queue_t *q,wait_queue_entry_t *e){(void)q;(void)e;}
void epoll_notify_event(vfs_node_t *n,uint32_t e){(void)n;(void)e;}
void vfs_node_init(vfs_node_t *n){memset(n,0,sizeof(*n));n->refcount=1;}
void vfs_close(vfs_node_t *n){if(!n)return;assert(n->refcount);if(--n->refcount==0){if(n->close)n->close(n);kfree(n);}}
static vfs_node_t *new_device(void){vfs_node_t *n=device_open(NULL);assert(n&&n->refcount==0);vfs_node_ref(n);return n;}
static struct fuse_request *queue(struct fuse_connection *c,unsigned cap){
 struct afuse_open_in in={0};struct fuse_request *r=request_new(AFUSE_OPEN,2,&in,sizeof(in),cap);assert(r&&!submit(c,r));return r;
}
int main(void){
 vfs_node_t *dev=new_device();struct fuse_connection *c=dev->device;
 unsigned char buf[1024];dev->flags|=FS_NONBLOCK;
 assert((int32_t)device_read(dev,0,sizeof(buf),buf)==-11);
 struct fuse_request *r=queue(c,16);uint64_t unique=r->unique;
 assert(device_poll(dev,POLLIN|POLLOUT)==(POLLIN|POLLOUT));
 assert((int32_t)device_read(dev,0,1,buf)==-22&&!r->sent);
 user=true;copy_fault=true;
 assert((int32_t)device_read(dev,0,sizeof(buf),buf)==-14&&!r->sent);
 copy_fault=false;assert(device_read(dev,0,sizeof(buf),buf)==48&&r->sent);
 struct afuse_out_header h={.len=16,.unique=unique+1};
 assert((int32_t)device_write(dev,0,16,(void*)&h)==-2);
 h.unique=unique;h.error=1;assert((int32_t)device_write(dev,0,16,(void*)&h)==-22);
 h.error=-4096;assert((int32_t)device_write(dev,0,16,(void*)&h)==-22);
 h.error=0;h.len=17;assert((int32_t)device_write(dev,0,16,(void*)&h)==-22);
 h.len=16;copy_fault=true;assert((int32_t)device_write(dev,0,16,(void*)&h)==-14&&!r->done);copy_fault=false;
 memcpy(buf,&h,16);((struct afuse_out_header*)buf)->len=40;
 assert((int32_t)device_write(dev,0,40,buf)==-22&&!r->done);
 h.error=-5;assert(device_write(dev,0,16,(void*)&h)==16&&r->done&&r->error==-5);
 assert((int32_t)device_write(dev,0,16,(void*)&h)==-2);
 spinlock_acquire(&c->lock);remove_locked(c,r);spinlock_release(&c->lock);req_put(r);
 /* A no-reply request is destroyed only after delivery. */
 struct afuse_forget_in forget={1};background(c,AFUSE_FORGET,2,&forget,sizeof(forget),true);
 assert(c->queued==1);assert(device_read(dev,0,sizeof(buf),buf)==48&&c->queued==0);
 /* Malformed INIT disconnects and completes outstanding foreground calls. */
 struct afuse_init_in init={7,26,65536,0};r=request_new(AFUSE_INIT,1,&init,sizeof(init),64);r->background=true;assert(!submit(c,r));
 assert(device_read(dev,0,sizeof(buf),buf)==56);unique=((struct afuse_in_header*)buf)->unique;
 struct fuse_request *waiting=queue(c,16);
 h=(struct afuse_out_header){.len=16,.unique=unique};assert(device_write(dev,0,16,(void*)&h)==16);
 assert(c->disconnected&&waiting->done&&waiting->error==-107);
 assert((int32_t)device_read(dev,0,sizeof(buf),buf)==-19);
 spinlock_acquire(&c->lock);remove_locked(c,waiting);spinlock_release(&c->lock);req_put(waiting);vfs_close(dev);
 assert(connection_count==0&&allocations==0);
 /* Last device close aborts queued work while inode/request owners retain c. */
 dev=new_device();c=dev->device;conn_get(c);r=queue(c,16);
 vfs_close(dev);assert(c->disconnected&&r->done);
 spinlock_acquire(&c->lock);remove_locked(c,r);spinlock_release(&c->lock);req_put(r);conn_put(c);
 assert(connection_count==0&&allocations==0);
 /* Bound queue allocation and validate numeric mount options. */
 dev=new_device();c=dev->device;struct fuse_request *pending[FUSE_MAX_REQUESTS];
 for(unsigned i=0;i<FUSE_MAX_REQUESTS;i++)pending[i]=queue(c,0);
 r=request_new(AFUSE_OPEN,2,NULL,0,0);assert(r&&submit(c,r)==-11);req_put(r);
 background(c,AFUSE_FORGET,2,&forget,sizeof(forget),true);assert(c->disconnected);
 for(unsigned i=0;i<FUSE_MAX_REQUESTS;i++){spinlock_acquire(&c->lock);remove_locked(c,pending[i]);spinlock_release(&c->lock);req_put(pending[i]);}
 vfs_close(dev);assert(!allocations&&!connection_count);
 uint32_t number;assert(!option_number("40000",5,8,&number)&&number==0040000);
 assert(option_number("4294967296",10,10,&number)==-22&&option_number("8",1,8,&number)==-22);
 alloc_fault=true;assert(!device_open(NULL)&&!connection_count);alloc_fault=false;
 puts("PASS: real FUSE framing, reply IDs/errors/lengths, user faults, queue limits, INIT failure, disconnect and zero allocation leaks");
}
