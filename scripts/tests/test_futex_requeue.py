#!/usr/bin/env python3
"""Exercise actual futex wait/wake/requeue code with deterministic races."""
from pathlib import Path
import os
import subprocess
import tempfile

source = Path(os.environ.get('FUTEX_SOURCE', 'kernel/src/syscalls/sys_futex.c')).read_text()

def function(signature):
    start = source.index(signature)
    end = source.index('{', start) + 1
    depth = 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end] + '\n'

prefix = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define EFAULT 14
#define EAGAIN 11
#define EINTR 4
#define EINVAL 22
#define ETIMEDOUT 110
#define THREAD_RUNNING 1
#define THREAD_BLOCKED 2
#define THREAD_READY 3
#define FUTEX_HASH_SIZE 256
#define FUTEX_BITSET_MATCH_ANY UINT32_MAX
#define LOCKDIAG_STAT(...) ((void)0)
typedef struct {bool held;} spinlock_t;
struct thread {int state; uint64_t wakeup_ticks,blocked_since_ms;
bool in_futex_wait,blocked_reported; unsigned futex_bucket;};
static struct thread self;
static uint64_t now;
static int mode, yields, wake_calls;
static bool cleanup_race, use_cmp;
static uint32_t words[256], *from, *to;
static void before_lock(spinlock_t *lock);
static void spinlock_acquire(spinlock_t *lock) {
 before_lock(lock); assert(!lock->held);lock->held=true;
}
static void spinlock_release(spinlock_t *lock) {assert(lock->held);lock->held=false;}
static struct thread *sched_get_current(void){return &self;}
static uint64_t lapic_timer_get_ticks(void){return now;}
static uint64_t rtc_get_boot_timestamp(void){return 0;}
static bool thread_has_pending_signal(struct thread *t){(void)t;return false;}
static void sched_wakeup(struct thread *t){wake_calls++;if(t->state==THREAD_BLOCKED)t->state=THREAD_READY;}
static void futex_bucket_note(unsigned bucket,bool woke){(void)bucket;(void)woke;}
static void fair_yield(void);
static void sched_yield(void){
 yields++;
 if(mode==3){fair_yield();self.state=THREAD_RUNNING;}
 else if(mode==1){now=self.wakeup_ticks;self.state=THREAD_RUNNING;cleanup_race=true;}
 else {assert(mode==2 && yields<=2);now=yields==1?1:self.wakeup_ticks;self.state=THREAD_RUNNING;}
}
'''
start = source.index('struct futex_key {')
end = source.index('/* Per-bucket wake traffic', start)
code = prefix + source[start:end]
code += function('static inline uint32_t futex_hash_key(')
code += function('static inline bool futex_key_equal(')
if 'static void futex_enqueue(' in source:
    code += function('static void futex_enqueue(')
if 'static uint32_t futex_lock_waiter(' in source:
    code += function('static uint32_t futex_lock_waiter(')
code += r'''
static int futex_get_key(uint32_t *uaddr,bool private,struct futex_key *key){
 (void)private;key->space=1;key->address=(uint64_t)uaddr;return 0;
}
'''
code += function('static int futex_wait_deadline(')
code += function('static uint64_t futex_wait(')
code += function('static inline uint64_t futex_wake_key(')
code += function('static uint64_t futex_requeue(')
code += function('static uint64_t futex_cmp_requeue(')
code += r'''
static struct futex_key key(uint32_t *word){struct futex_key k;futex_get_key(word,true,&k);return k;}
static struct thread fair_old;
static void fair_yield(void){
 unsigned bucket=futex_hash_key(key(from));
 assert(futex_hash[bucket].head->thread==&fair_old);
 assert(futex_wake_key(key(from),1,UINT32_MAX)==1);
 assert(fair_old.state==THREAD_READY && self.state==THREAD_BLOCKED);
 assert(futex_wake_key(key(from),1,UINT32_MAX)==1 && self.state==THREAD_READY);
}
static void before_lock(spinlock_t *lock){
 if(cleanup_race && lock==&futex_hash[futex_hash_key(key(from))].lock){
  cleanup_race=false;
  uint64_t n=use_cmp?futex_cmp_requeue(from,0,1,to,0,true):futex_requeue(from,0,1,to,true);
  assert(n==1);
 }
}
static void reset(void){memset(futex_hash,0,sizeof(futex_hash));memset(&self,0,sizeof(self));
 self.state=THREAD_RUNNING;now=0;yields=0;wake_calls=0;cleanup_race=false;}
int main(int argc,char **argv){
 (void)argc;
 bool wake_only=argv[1] && !strcmp(argv[1],"wake");
 from=&words[0];to=&words[1];
 while(futex_hash_key(key(from))==futex_hash_key(key(to)))to++;
 if(!wake_only) {
 for(int cmp=0;cmp<2;cmp++){
  reset();mode=1;use_cmp=cmp;uint64_t ts[2]={0,10000000};
  assert((int64_t)futex_wait(from,0,ts,true,false,false,UINT32_MAX)==-ETIMEDOUT);
  for(unsigned i=0;i<FUTEX_HASH_SIZE;i++)assert(!futex_hash[i].head);
 }
 puts("PASS: timeout racing REQUEUE and CMP_REQUEUE removes the relocated stack waiter");
 reset();mode=2;uint64_t ts[2]={0,10000000};
 assert((int64_t)futex_wait(from,0,ts,true,false,false,UINT32_MAX)==-ETIMEDOUT && yields==2 && now==10);
 puts("PASS: unrelated scheduler wake retains a timed waiter until its deadline");
 }
 for(int operation=0;operation<3;operation++){
  reset();struct thread runnable={.state=THREAD_READY},blocked={.state=THREAD_BLOCKED};
  struct futex_waiter second={.key=key(from),.thread=&blocked,.bitset=UINT32_MAX};
  struct futex_waiter first={.key=key(from),.thread=&runnable,.bitset=UINT32_MAX,.next=&second};
  unsigned b=futex_hash_key(key(from));futex_hash[b].head=&first;
  uint64_t result=operation==0?futex_wake_key(key(from),1,UINT32_MAX):
   operation==1?futex_requeue(from,1,0,to,true):futex_cmp_requeue(from,1,0,to,0,true);
  assert(result==1 && futex_hash[b].head==&second && blocked.state==THREAD_BLOCKED && wake_calls==1);
 }
 puts("PASS: WAKE and both REQUEUE variants count a runnable registered waiter against the wake limit");
 reset();mode=3;fair_old=(struct thread){.state=THREAD_BLOCKED};
 unsigned bucket=futex_hash_key(key(from));
 struct futex_waiter old={.key=key(from),.bucket=bucket,.thread=&fair_old,.bitset=UINT32_MAX};
 futex_hash[bucket].head=&old;
 assert(futex_wait(from,0,NULL,true,false,false,UINT32_MAX)==0 && yields==1);
 assert(!futex_hash[bucket].head);
 puts("PASS: a new WAIT cannot steal the next wake from an older registered waiter");
 for(int cmp=0;cmp<2;cmp++)for(int collide=0;collide<2;collide++){
  reset();use_cmp=cmp;to=&words[1];
  while((futex_hash_key(key(from))==futex_hash_key(key(to)))!=collide)to++;
  assert(to<&words[256]);
  unsigned dst=futex_hash_key(key(to)),src=futex_hash_key(key(from));
  struct thread t0={.state=THREAD_BLOCKED},t1=t0,t2=t0;
  struct futex_waiter w0={.key=key(to),.bucket=dst,.thread=&t0,.bitset=UINT32_MAX};
  struct futex_waiter w2={.key=key(from),.bucket=src,.thread=&t2,.bitset=UINT32_MAX};
  struct futex_waiter w1={.key=key(from),.bucket=src,.thread=&t1,.bitset=UINT32_MAX,.next=&w2};
  futex_hash[dst].head=&w0;
  if(collide)w0.next=&w1;else futex_hash[src].head=&w1;
  assert((cmp?futex_cmp_requeue(from,0,2,to,0,true):futex_requeue(from,0,2,to,true))==2);
  assert(futex_hash[dst].head==&w0 && w0.next==&w1 && w1.next==&w2 && !w2.next);
  assert(futex_wake_key(key(to),1,UINT32_MAX)==1 && t0.state==THREAD_READY);
  assert(t1.state==THREAD_BLOCKED && t2.state==THREAD_BLOCKED);
  assert(futex_wake_key(key(to),1,UINT32_MAX)==1 && t1.state==THREAD_READY && t2.state==THREAD_BLOCKED);
  assert(futex_wake_key(key(to),1,UINT32_MAX)==1 && t2.state==THREAD_READY);
 }
 puts("PASS: REQUEUE/CMP_REQUEUE preserve FIFO order behind destination waiters, including hash collisions");
}
'''
with tempfile.TemporaryDirectory() as tmp:
    path = Path(tmp)
    (path / 'test.c').write_text(code)
    subprocess.run(['cc', '-O1', '-g', '-fsanitize=address,undefined',
                    str(path / 'test.c'), '-o', str(path / 'test')], check=True)
    subprocess.run([str(path / 'test'), os.environ.get('FUTEX_CASE', 'all')], check=True,
                   env={**os.environ, 'ASAN_OPTIONS': 'detect_leaks=0'})
