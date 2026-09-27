#include "net/raw_icmp.h"
#include "lib/string.h"
#include "console/klog.h"
#include "lock/spinlock.h"
#include "net/ipv4.h"
#include "sched/sched.h"
#include "socket/epoll.h"
static struct raw_icmp_socket sockets[RAW_ICMP_MAX_SOCKETS];
static spinlock_t lock=SPINLOCK_INIT;


void raw_icmp_init(void) {
  spinlock_init(&lock);
  memset(sockets,0,sizeof(sockets));
}

struct raw_icmp_socket *raw_icmp_alloc(void){
  spinlock_acquire(&lock);
  for(int i=0;i<RAW_ICMP_MAX_SOCKETS;i++)if(!sockets[i].used){
    memset(&sockets[i],0,sizeof(sockets[i]));
    sockets[i].used=true;
    spinlock_release(&lock);
    return&sockets[i];
  }
  spinlock_release(&lock);
  return NULL;
}

void raw_icmp_free(struct raw_icmp_socket*s) {
  if(!s)return;
  spinlock_acquire(&lock);
  memset(s,0,sizeof(*s));
  spinlock_release(&lock);
 }


int raw_icmp_send(struct raw_icmp_socket*s,const void*p,size_t n,uint32_t dst){
 if(!s||!p||!dst)return-22;
 if(n>1480)return-90;
 int r=ipv4_send_raw(dst,1,p,n);return r<0?r:(int)n;
}
ssize_t raw_icmp_recv(struct raw_icmp_socket*s,void*buf,size_t n,uint32_t*src,bool nb) {
 if(!s||!buf)return-22;
 for(;;){spinlock_acquire(&lock);
  if(s->tail!=s->head)
  {struct raw_icmp_packet*p=&s->queue[s->tail%RAW_ICMP_QUEUE_DEPTH];
   size_t copy=p->length<n?p->length:n;memcpy(buf,p->data,copy);if(src)*src=p->src;s->tail++;
   spinlock_release(&lock);return(ssize_t)copy;}spinlock_release(&lock);
  struct thread *current=sched_get_current();
  if(nb)return-11;
  /* Check for pending signals before blocking so we don't miss one that
   * arrived between the empty-queue check and the yield below. */
  if(thread_has_pending_signal(current))return-4; /* -EINTR */
  if(s->wait){struct thread*t=current;
   wait_queue_entry_t e={.thread=t,.next=NULL};wait_queue_add(s->wait,&e);
   spinlock_acquire(&lock);bool empty=s->tail==s->head;if(empty)t->state=THREAD_BLOCKED;spinlock_release(&lock);
   if(!empty)wait_queue_wake_one(s->wait);sched_yield();wait_queue_remove(s->wait,&e);
   /* Woken up — if a signal is pending (e.g. SIGALRM) return EINTR so the
    * syscall exit path can deliver it before ping loops back. */
   if(thread_has_pending_signal(t))return-4; /* -EINTR */
  }else sched_yield();
 }
}
bool raw_icmp_readable(struct raw_icmp_socket*s) {
  if(!s)return false;
  spinlock_acquire(&lock);
  bool r=s->head!=s->tail;
  spinlock_release(&lock);
  return r;
}

void raw_icmp_deliver(uint32_t src,uint32_t dst,const uint8_t*p,size_t n){

 spinlock_acquire(&lock);for(int i=0;i<RAW_ICMP_MAX_SOCKETS;i++){struct raw_icmp_socket*s=&sockets[i];

  if(!s->used||(s->local_ip&&s->local_ip!=dst)||(s->connected&&s->remote_ip!=src))continue;
  if(s->head-s->tail>=RAW_ICMP_QUEUE_DEPTH)continue;
  struct raw_icmp_packet*q=&s->queue[s->head%RAW_ICMP_QUEUE_DEPTH];
  q->src=src;q->length=(uint16_t)(n>sizeof(q->data)
  ?sizeof(q->data):n);memcpy(q->data,p,q->length);s->head++;
  if(s->wait)wait_queue_wake_one(s->wait);
  if(s->node)epoll_notify_event(s->node,1);
 }
 spinlock_release(&lock);
}
