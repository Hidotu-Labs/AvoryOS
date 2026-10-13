#!/usr/bin/env python3
"""Re-listening must retain ownership of pending accepted sockets."""
from pathlib import Path
import subprocess, tempfile, os
root=Path(__file__).resolve().parents[2]
s=(root/'kernel/src/socket/af_unix_connect.c').read_text()
a=s.index('int unix_listen_impl(');b=s.index('{',a)+1;depth=1
while depth:
    depth+=(s[b]=='{')-(s[b]=='}');b+=1
fn=s[a:b]
pre=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#define SS_CONNECTED 1
#define SS_LISTENING 2
struct unix_sock {bool is_listener;int backlog,accept_queue_len;struct unix_sock *accept_next;};
typedef struct unix_sock unix_sock_t;
typedef struct {int held;} spinlock_t;
typedef struct {void *sk;int state;spinlock_t lock;} socket_t;
static void spinlock_acquire(spinlock_t *l){assert(!l->held);l->held=1;}
static void spinlock_release(spinlock_t *l){assert(l->held);l->held=0;}
#define klog_debug_puts(x) ((void)(x))
#define klog_debug_uint64(x) ((void)(x))
'''
post=r'''
int main(void){
 assert(unix_listen_impl(NULL,1)==-22);
 socket_t invalid={0};assert(unix_listen_impl(&invalid,1)==-22);
 for(unsigned iteration=0;iteration<1000;iteration++){
  unix_sock_t u={0};socket_t s={.sk=&u};
  assert(!unix_listen_impl(&s,8));assert(u.is_listener && s.state==SS_LISTENING);
  unix_sock_t *one=calloc(1,sizeof(*one)),*two=calloc(1,sizeof(*two));
  assert(one && two);one->accept_next=two;u.accept_next=one;u.accept_queue_len=2;
  assert(!unix_listen_impl(&s,1));assert(u.backlog==1);
  assert(u.accept_next==one && u.accept_queue_len==2);
  assert(!unix_listen_impl(&s,16));assert(u.accept_next==one && u.accept_queue_len==2);
  // Accept one entry, then listener teardown owns the remaining entry.
  u.accept_next=one->accept_next;u.accept_queue_len--;free(one);
  assert(u.accept_next==two && u.accept_queue_len==1);free(u.accept_next);
 }
 unix_sock_t connected={0};socket_t c={.sk=&connected,.state=SS_CONNECTED};
 assert(unix_listen_impl(&c,8)==-22 && !connected.is_listener && c.state==SS_CONNECTED && !c.lock.held);
 puts("PASS: 1000 pending-queue lifecycles survive backlog updates; connected sockets rejected");
}
'''
with tempfile.TemporaryDirectory(prefix='unix-listen-lifetime-') as d:
    source=Path(d)/'test.c';binary=Path(d)/'test'
    source.write_text(pre+fn+post)
    subprocess.run(['cc','-O1','-g','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',str(source),'-o',str(binary)],check=True)
    subprocess.run([str(binary)],env=dict(os.environ,ASAN_OPTIONS='detect_leaks=0'),check=True,timeout=20)
    old=fn.replace('if (!usk->is_listener) {','{')
    source.write_text(pre+old+post)
    subprocess.run(['cc','-O1',str(source),'-o',str(binary)],check=True)
    result=subprocess.run([str(binary)],capture_output=True,timeout=20)
    assert result.returncode!=0,'Old queue-reset behavior was not detected'
    print('PASS: original re-listen queue reset fails the regression')
