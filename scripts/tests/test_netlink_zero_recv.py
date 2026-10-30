#!/usr/bin/env python3
"""Exercise libtorrent's zero-buffer Netlink size probe through real receive code."""
from pathlib import Path
import subprocess
import os
import tempfile
ROOT = Path(__file__).resolve().parents[2]
def extract(path, signature):
    source = (ROOT / path).read_text()
    start = source.index(signature)
    end = source.index('{', start) + 1
    depth = 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end] + '\n'
code = r'''
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <sys/types.h>
#define AF_INET 2
#define AF_INET6 10
#define AF_NETLINK 16
#define SOCK_NONBLOCK 2048
#define MSG_PEEK 2
#define MSG_TRUNC 32
#define MSG_DONTWAIT 64
#define THREAD_BLOCKED 1
#define THREAD_RUNNING 0
struct sockaddr {unsigned short family; char data[14];};
struct sockaddr_nl {unsigned short nl_family,pad; unsigned nl_pid,nl_groups;};
struct thread {int state;};
typedef struct {void *thread,*next,*wq,*thread_next,*thread_prev;} wait_queue_entry_t;
typedef int wait_queue_t;
typedef struct skb {struct skb *next; size_t len; void *data; unsigned nl_portid,nl_groups,nl_pid,nl_uid,nl_gid;} sk_buff_t;
typedef struct {struct {int lock; sk_buff_t *head,*tail; int len;} recv_queue;} netlink_sock_t;
struct netlink_sender {uint32_t pid,uid,gid,groups;};
typedef struct socket socket_t;
struct ops {ssize_t (*recvfrom)(socket_t*,void*,size_t,int,struct sockaddr*,int*); ssize_t (*recv)(socket_t*,void*,size_t,int);};
struct socket {int domain,flags;bool closing;void *sk,*wait_queue,*net_ns;struct ops *ops;};
static void spinlock_acquire(int *p){assert(!*p);*p=1;}
static void spinlock_release(int *p){assert(*p);*p=0;}
static struct thread *sched_get_current(void){return NULL;}
static void sched_yield(void){assert(0);}
static bool thread_has_pending_signal(struct thread *t){(void)t;return false;}
static void wait_queue_add(wait_queue_t *q,wait_queue_entry_t *e){(void)q;(void)e;}
static void wait_queue_remove(wait_queue_t *q,wait_queue_entry_t *e){(void)q;(void)e;}
static unsigned freed;
static void free_skb(sk_buff_t *s){(void)s;freed++;}
static socket_t test_socket;
static unsigned refs;
static socket_t *socket_from_fd(int fd){if(fd!=9)return NULL;refs++;return &test_socket;}
static void socket_release_fd(socket_t **s){if(*s){assert(refs);refs--;}}
static uint64_t socket_eno_notsock(int fd){(void)fd;return (uint64_t)-9;}
static bool is_user_ptr(uint64_t p){return p>=4096;}
static int copy_from_user(void *d,const void *s,size_t n){memcpy(d,s,n);return 0;}
static int copy_to_user(void *d,const void *s,size_t n){memcpy(d,s,n);return 0;}
'''
for path, signatures in [
    ('kernel/src/socket/af_netlink.c', ['static ssize_t netlink_recvfrom_full(', 'static ssize_t netlink_recvfrom(', 'static ssize_t netlink_recv(']),
    ('kernel/src/socket/socket.c', ['ssize_t socket_recv(', 'ssize_t socket_recvfrom(']),
    ('kernel/src/syscalls/sys_socket.c', ['static uint64_t sys_recvfrom(']),
]:
    for signature in signatures:
        code += extract(path, signature)
code += r'''
int main(void){
 char payload[48]="netlink interface reply",output[48]={0};
 sk_buff_t packet={.len=sizeof(payload),.data=payload};
 netlink_sock_t state={.recv_queue={.head=&packet,.tail=&packet,.len=1}};
 struct ops ops={.recvfrom=netlink_recvfrom,.recv=netlink_recv};
 test_socket=(socket_t){.domain=AF_NETLINK,.sk=&state,.ops=&ops};
 assert(sys_recvfrom(9,0,0,MSG_PEEK|MSG_TRUNC,0,0)==48);
 assert(state.recv_queue.len==1 && !freed && !refs);
 assert(socket_recv(&test_socket,NULL,0,MSG_PEEK|MSG_TRUNC)==48);
 assert(sys_recvfrom(9,(uintptr_t)output,sizeof(output),MSG_TRUNC,0,0)==48);
 assert(!memcmp(output,payload,48) && freed==1 && !refs);
 assert(sys_recvfrom(9,0,0,MSG_PEEK|MSG_TRUNC|MSG_DONTWAIT,0,0)==(uint64_t)-11);
 assert(sys_recvfrom(77,0,0,MSG_PEEK|MSG_TRUNC,0,0)==(uint64_t)-9);
 test_socket.domain=AF_INET;
 assert(socket_recvfrom(&test_socket,NULL,0,0,NULL,NULL)==0);
 puts("PASS: Netlink zero-byte size probe, peek preservation, receive, empty queue and FD validation");
}
'''
with tempfile.TemporaryDirectory(prefix='avory-netlink-zero-') as td:
    path=Path(td)
    (path/'test.c').write_text(code)
    subprocess.run(['cc','-std=gnu11','-g','-fsanitize=address,undefined',str(path/'test.c'),'-o',str(path/'test')],check=True)
    subprocess.run([str(path/'test')],check=True, env={**os.environ, 'ASAN_OPTIONS': os.environ.get('ASAN_OPTIONS', 'detect_leaks=0')})
