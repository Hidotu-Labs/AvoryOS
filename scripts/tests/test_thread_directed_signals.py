#!/usr/bin/env python3
"""Exercise actual signal consumers at glibc setxid and signalfd boundaries."""
from pathlib import Path
import subprocess
import tempfile

source = (Path(__file__).resolve().parents[2] / 'kernel/src/syscalls/sys_signal.c').read_text()
def function(signature):
    start = source.index(signature)
    brace = source.index('{', start)
    depth, end = 1, brace + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]

code = r'''
#include <stdint.h>
#include <stdbool.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>
struct thread {
 uint32_t tgid;
 uint64_t pending_signals,thread_directed_signals;
 uint32_t signal_sender_pid[64];
 struct thread *global_next;
};
typedef int spinlock_t;
spinlock_t tid_lock;
struct thread *global_thread_list;
static void spinlock_acquire(spinlock_t *l){assert(!*l);*l=1;}
static void spinlock_release(spinlock_t *l){assert(*l);*l=0;}
'''
for name in ('static void signal_dequeue_locked(',
             'static uint64_t get_pending_signals_tgid(',
             'static bool consume_signal_tgid('):
    code += function(name) + '\n'
code += r'''
int main(void){
 struct thread t[4]={0};uint64_t bit=1ULL<<32;uint32_t sender;
 for(int i=0;i<4;i++){t[i].tgid=10;t[i].global_next=i<3?&t[i+1]:NULL;}
 global_thread_list=t;
 /* Every glibc worker must acknowledge its own SIGSETXID. */
 for(int i=0;i<4;i++){t[i].pending_signals=bit;t[i].thread_directed_signals=bit;}
 for(int i=0;i<4;i++){
  signal_dequeue_locked(&t[i],bit);
  assert(!t[i].pending_signals&&!t[i].thread_directed_signals);
  for(int j=i+1;j<4;j++)assert(t[j].pending_signals==bit);
 }
 /* Process-wide replicas still produce one delivery. */
 for(int i=0;i<4;i++)t[i].pending_signals=bit;
 signal_dequeue_locked(&t[1],bit);
 for(int i=0;i<4;i++)assert(!t[i].pending_signals);
 /* A sibling's signalfd cannot steal a thread-targeted signal. */
 t[2].pending_signals=t[2].thread_directed_signals=bit;
 t[2].signal_sender_pid[32]=42;
 assert(!get_pending_signals_tgid(&t[0],bit));
 assert(!consume_signal_tgid(&t[0],33,&sender));
 assert(get_pending_signals_tgid(&t[2],bit)==bit);
 assert(consume_signal_tgid(&t[2],33,&sender)&&sender==42);
 assert(!t[2].pending_signals&&!t[2].thread_directed_signals);
 /* Consuming a group signal must preserve independent directed copies. */
 for(int i=0;i<4;i++)t[i].pending_signals=bit;
 t[2].thread_directed_signals=bit;
 assert(consume_signal_tgid(&t[0],33,&sender));
 for(int i=0;i<4;i++)assert(t[i].pending_signals==(i==2?bit:0));
 puts("PASS: per-worker setxid delivery, single group delivery, signalfd ownership and mixed recipients");
}
'''
with tempfile.TemporaryDirectory(prefix='avory-directed-signals-') as tmp:
    c = Path(tmp) / 'check.c'
    executable = Path(tmp) / 'check'
    c.write_text(code)
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', str(c), '-o', str(executable)], check=True)
    subprocess.run([str(executable)], check=True)
