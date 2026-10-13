#!/usr/bin/env python3
"""Actual Netlink options and socket-name syscalls with fault-safe usercopy mocks."""
from pathlib import Path
import os, subprocess, tempfile

def extract(path,signature):
 s=Path(path).read_text();a=s.index(signature);b=s.index('{',a)+1;d=1
 while d:
  d+=(s[b]=='{')-(s[b]=='}');b+=1
 return s[a:b]+'\n'
code=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include "arch/uaccess.h"
#define AF_NETLINK 16
#define SOL_SOCKET 1
#define SOL_NETLINK 270
#define NETLINK_EXT_ACK 11
#define NETLINK_ADD_MEMBERSHIP 1
#define NETLINK_DROP_MEMBERSHIP 2
#define SO_KEEPALIVE 9
#define SO_PASSCRED 16
#define SO_RCVBUF 8
#define SO_RCVBUFFORCE 33
#define SO_SNDBUF 7
#define SO_SNDBUFFORCE 32
#define SO_REUSEADDR 2
#define SO_BROADCAST 6
#define SO_RCVTIMEO 20
#define SO_SNDTIMEO 21
#define SO_LINGER 13
struct sockaddr {uint16_t family;char data[14];};
struct sockaddr_nl {uint16_t nl_family,nl_pad;uint32_t nl_pid,nl_groups;};
typedef struct socket socket_t;
struct sock_ops {int (*setsockopt)(socket_t *,int,int,const void *,int);};
struct socket {void *sk;bool closing;int rcvbuf,sndbuf,reuseaddr;struct sock_ops *ops;};
typedef struct {uintptr_t portid;uint32_t groups;bool passcred,keepalive,ext_ack;} netlink_sock_t;
static netlink_sock_t state={.portid=42,.groups=3};
static socket_t socket={.sk=&state};
static unsigned refs,pushes;
static bool bad_from,bad_to;
static socket_t *socket_from_fd(int fd){if(fd!=9)return NULL;refs++;return &socket;}
static uint64_t socket_eno_notsock(int fd){(void)fd;return (uint64_t)-9;}
static void socket_fd_put(socket_t *s){(void)s;assert(refs);refs--;}
static bool vmm_is_user_addr_range_writable(uint64_t a,size_t n){(void)a;(void)n;return false;}
unsigned long copy_from_user(void *to,const void *from,unsigned long n){if(bad_from || (uintptr_t)from<4096)return n;memcpy(to,from,n);return 0;}
unsigned long copy_to_user(void *to,const void *from,unsigned long n){if(bad_to || (uintptr_t)to<4096)return n;memcpy(to,from,n);return 0;}
static void netlink_push_fake_uevent(socket_t *s){(void)s;pushes++;}
'''
for sig in ['static int netlink_getsockname(', 'static int netlink_setsockopt(', 'static int netlink_getsockopt(']:
 code+=extract('kernel/src/socket/af_netlink.c',sig)
code+=r'''
static int socket_getsockname(socket_t *s,struct sockaddr *a,int *n){return netlink_getsockname(s,a,n);}
static int socket_getpeername(socket_t *s,struct sockaddr *a,int *n){return netlink_getsockname(s,a,n);}
'''
for sig in ['static void socket_release_fd(', 'static int copy_sockaddr_out(', 'static uint64_t sys_getsockname(', 'static uint64_t sys_getpeername(', 'static uint64_t sys_setsockopt(']:
 code+=extract('kernel/src/syscalls/sys_socket.c',sig)
code+=r'''
int main(void){
 struct sock_ops ops={.setsockopt=netlink_setsockopt};socket.ops=&ops;
 int val=-1,len=4;
 assert(!state.ext_ack);
 val=7;assert(!sys_setsockopt(9,SOL_NETLINK,NETLINK_EXT_ACK,(uintptr_t)&val,4,0) && state.ext_ack);
 val=0;len=4;assert(!netlink_getsockopt(&socket,SOL_NETLINK,NETLINK_EXT_ACK,&val,&len) && val==1 && len==4);
 val=0;assert(!sys_setsockopt(9,SOL_NETLINK,NETLINK_EXT_ACK,(uintptr_t)&val,4,0) && !state.ext_ack);
 assert((int64_t)sys_setsockopt(9,SOL_NETLINK,NETLINK_EXT_ACK,(uintptr_t)&val,3,0)==-22);
 bad_from=true;assert((int64_t)sys_setsockopt(9,SOL_NETLINK,NETLINK_EXT_ACK,(uintptr_t)&val,4,0)==-14);bad_from=false;
 assert(!state.ext_ack);
 len=4;bad_to=true;assert(netlink_getsockopt(&socket,SOL_NETLINK,NETLINK_EXT_ACK,&val,&len)==-14);bad_to=false;
 assert(netlink_getsockopt(&socket,999,NETLINK_EXT_ACK,&val,&len)==-92);
 assert((int64_t)sys_setsockopt(9,SOL_NETLINK,999,(uintptr_t)&val,4,0)==-92);

 assert((int64_t)sys_setsockopt(9,SOL_SOCKET,SO_KEEPALIVE,(uintptr_t)&val,1,0)==-22);
 bad_from=true;assert((int64_t)sys_setsockopt(9,SOL_SOCKET,SO_KEEPALIVE,(uintptr_t)&val,4,0)==-14);bad_from=false;
 val=1;assert(!sys_setsockopt(9,SOL_SOCKET,SO_KEEPALIVE,(uintptr_t)&val,4,0) && state.keepalive);
 val=0;assert(!sys_setsockopt(9,SOL_SOCKET,SO_KEEPALIVE,(uintptr_t)&val,4,0) && !state.keepalive);
 val=-1;assert(!netlink_getsockopt(&socket,SOL_SOCKET,SO_KEEPALIVE,&val,&len) && val==0 && len==4);
 val=1;assert(!netlink_setsockopt(&socket,SOL_SOCKET,SO_KEEPALIVE,&val,4));val=0;len=4;
 assert(!netlink_getsockopt(&socket,SOL_SOCKET,SO_KEEPALIVE,&val,&len) && val==1);
 val=0;assert(!netlink_setsockopt(&socket,SOL_SOCKET,SO_KEEPALIVE,&val,4));assert(!state.keepalive);
 assert(netlink_setsockopt(&socket,SOL_SOCKET,SO_KEEPALIVE,&val,1)==-22);
 bad_from=true;assert(netlink_setsockopt(&socket,SOL_SOCKET,SO_KEEPALIVE,&val,4)==-14);bad_from=false;
 state.passcred=true;assert(!netlink_getsockopt(&socket,SOL_SOCKET,SO_PASSCRED,&val,&len) && val==1);
 assert(netlink_getsockopt(&socket,SOL_SOCKET,999,&val,&len)==-92);
 len=-1;assert(netlink_getsockopt(&socket,SOL_SOCKET,SO_KEEPALIVE,&val,&len)==-22);
 len=1;val=-1;assert(!netlink_getsockopt(&socket,SOL_SOCKET,SO_KEEPALIVE,&val,&len) && len==1);
 int group=0;assert(netlink_setsockopt(&socket,SOL_NETLINK,NETLINK_ADD_MEMBERSHIP,&group,4)==-22);
 group=33;assert(netlink_setsockopt(&socket,SOL_NETLINK,NETLINK_DROP_MEMBERSHIP,&group,4)==-22);
 group=32;assert(!netlink_setsockopt(&socket,SOL_NETLINK,NETLINK_ADD_MEMBERSHIP,&group,4) && state.groups==(3u|0x80000000u));
 unsigned char expected[12];struct sockaddr_nl nl={.nl_family=AF_NETLINK,.nl_pid=42,.nl_groups=state.groups};memcpy(expected,&nl,12);
 for(unsigned which=0;which<2;which++){
  for(int capacity=0;capacity<=20;capacity++){
   unsigned char output[32];memset(output,0xa5,sizeof(output));len=capacity;
   int64_t rc=which?sys_getpeername(9,(uintptr_t)output,(uintptr_t)&len,0,0,0):sys_getsockname(9,(uintptr_t)output,(uintptr_t)&len,0,0,0);
   assert(rc==0 && len==12 && !refs);unsigned copied=capacity<12?capacity:12;
   assert(!memcmp(output,expected,copied));for(unsigned j=copied;j<32;j++)assert(output[j]==0xa5);
  }
  len=0;assert(!sys_getsockname(9,0,(uintptr_t)&len,0,0,0) && len==12 && !refs);
  len=12;assert((int64_t)sys_getsockname(9,0,(uintptr_t)&len,0,0,0)==-14 && !refs);
  assert((int64_t)sys_getsockname(9,0,0,0,0,0)==-14 && !refs);
  unsigned char output[32];len=12;bad_from=true;
  assert((int64_t)sys_getsockname(9,(uintptr_t)output,(uintptr_t)&len,0,0,0)==-14 && !refs);bad_from=false;bad_to=true;
  assert((int64_t)sys_getsockname(9,(uintptr_t)output,(uintptr_t)&len,0,0,0)==-14 && !refs);bad_to=false;
  len=-1;assert((int64_t)sys_getsockname(9,(uintptr_t)output,(uintptr_t)&len,0,0,0)==-22 && !refs);
  assert((int64_t)sys_getsockname(9,UINT64_MAX,(uintptr_t)&len,0,0,0)==-14 && !refs);
 }
 puts("PASS: Netlink extended ACK and keepalive state, unsupported options, fault/length errors and membership bounds; actual name syscalls return full lengths for truncated/zero buffers, copy faults still EFAULT, refs released");
}
'''
with tempfile.TemporaryDirectory() as tmp:
 p=Path(tmp);(p/'test.c').write_text(code)
 subprocess.run(['cc','-O1','-g','-fsanitize=address,undefined','-I','kernel/src/include',str(p/'test.c'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True,env={**os.environ,'ASAN_OPTIONS':'detect_leaks=0'})
