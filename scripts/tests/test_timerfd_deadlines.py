#!/usr/bin/env python3
"""Exercise the actual timerfd tick: early IRQs, lost arms and periodic phase."""
from pathlib import Path
import os,subprocess,tempfile,sys
source=Path(sys.argv[1] if len(sys.argv)>1 else 'kernel/src/syscalls/sys_aio.c').read_text()
function=source[source.index('void timerfd_tick(void) {'):source.index('\nstatic uint32_t timerfd_read')]
code=r'''
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#define TIMERFD_MAX 4
#define POLLIN 1
#define __ATOMIC_RELAXED 0
struct vfs_node {int unused;};
typedef struct vfs_node vfs_node_t;
typedef struct {bool busy;} spinlock_t;
static bool spinlock_try_acquire(spinlock_t *p){if(p->busy)return false;p->busy=true;return true;}
static void spinlock_release(spinlock_t *p){assert(p->busy);p->busy=false;}
typedef struct {uint64_t expire_ms,interval_sec,interval_nsec,expirations;spinlock_t lock;vfs_node_t *node;int wq;} timerfd_ctx_t;
static timerfd_ctx_t *timerfd_table[TIMERFD_MAX];
static spinlock_t timerfd_table_lock;
static unsigned timerfd_active_count;
static uint64_t now,armed;
static unsigned notifications,wakes;
uint64_t lapic_timer_get_ms(void){return now;}
void lapic_timer_rearm_if_earlier(uint64_t t){if(!armed||t<armed)armed=t;}
void epoll_notify_event(vfs_node_t *n,uint32_t e){assert(n&&e==1);notifications++;}
void wait_queue_wake_all(void *p){assert(p);wakes++;}
'''+function+r'''
int main(void){
 vfs_node_t node={0};timerfd_ctx_t a={.expire_ms=20,.node=&node},b={.expire_ms=35,.node=&node};
 timerfd_table[0]=&a;timerfd_table[1]=&b;timerfd_active_count=2;
 now=1;timerfd_tick();assert(armed==20&&!notifications&&!a.expirations);
 armed=0;now=20;timerfd_tick();assert(armed==35&&a.expirations==1&&!a.expire_ms);
 assert(notifications==1&&wakes==1);
 a.expire_ms=20;a.interval_nsec=10000000;a.expirations=0;
 armed=0;now=65;timerfd_tick();assert(a.expirations==5&&a.expire_ms==70&&armed==70);
 assert(b.expirations==1&&!b.expire_ms);
 armed=0;now=66;a.lock.busy=true;timerfd_tick();assert(armed==67);a.lock.busy=false;
 armed=0;timerfd_table_lock.busy=true;timerfd_tick();assert(armed==67);timerfd_table_lock.busy=false;
 armed=0;now=67;timerfd_tick();assert(armed==70);
 puts("PASS: early IRQ retains timerfd deadline, periodic overrun count/phase, contended-lock retry");
}
'''
with tempfile.TemporaryDirectory(prefix='avory-timerfd-') as d:
 p=Path(d);(p/'test.c').write_text(code)
 subprocess.run([os.environ.get('CC','cc'),'-std=gnu11','-fsanitize=address,undefined','-g',p/'test.c','-o',p/'test'],check=True)
 subprocess.run([p/'test'],check=True)
