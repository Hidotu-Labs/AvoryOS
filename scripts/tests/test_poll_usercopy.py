#!/usr/bin/env python3
"""Actual poll/ppoll syscall source with failing and mutable user-memory mocks."""
from pathlib import Path
import subprocess, tempfile, os
ROOT=Path(__file__).resolve().parents[2]
def extract(signature):
 s=(ROOT/'kernel/src/syscalls/sys_poll.c').read_text();a=s.index(signature);b=s.index('{',a)+1;d=1
 while d:d+=(s[b]=='{')-(s[b]=='}');b+=1
 return s[a:b]+'\n'
code=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "arch/uaccess.h"
#define MAX_FDS 1024
struct pollfd {int fd;short events,revents;};
struct thread {uint64_t signal_mask,last_poll_nfds;int64_t last_poll_sec,last_poll_nsec;};
static struct thread self={.signal_mask=123};
static struct thread *sched_get_current(void){return &self;}
static bool fail_read,fail_write,fail_alloc,mutate;
static unsigned allocations,calls;
static uint64_t seen_timeout,seen_mask,answer;
static struct pollfd *user_fds;
static void *kmalloc(size_t n){if(fail_alloc)return NULL;allocations++;return malloc(n);}
static void kfree(void *p){assert(allocations);allocations--;free(p);}
unsigned long copy_from_user(void *to,const void *from,unsigned long n){if(fail_read || (uintptr_t)from<4096)return n;memcpy(to,from,n);return 0;}
unsigned long copy_to_user(void *to,const void *from,unsigned long n){if(fail_write || (uintptr_t)to<4096)return n;memcpy(to,from,n);return 0;}
static uint64_t do_poll(struct pollfd *fds,uint64_t n,uint64_t timeout){
 calls++;seen_timeout=timeout;seen_mask=self.signal_mask;
 if(n){assert(fds!=user_fds && fds[0].fd==9);if(mutate){user_fds[0].fd=77;user_fds[0].events=4;assert(fds[0].fd==9 && fds[0].events==1);}fds[0].revents=1;}
 return answer;
}
'''+extract('static uint64_t sys_poll(')+extract('static uint64_t sys_ppoll(')+r'''
int main(void){
 struct pollfd fd={9,1,0};user_fds=&fd;answer=1;mutate=true;
 assert(sys_poll((uintptr_t)&fd,1,0,0,0,0)==1);
 assert(fd.fd==77 && fd.events==4 && fd.revents==1 && !allocations);
 mutate=false;fd=(struct pollfd){9,1,0};
 fail_read=true;assert(sys_poll((uintptr_t)&fd,1,0,0,0,0)==(uint64_t)-14);fail_read=false;
 fail_write=true;assert(sys_poll((uintptr_t)&fd,1,0,0,0,0)==(uint64_t)-14);fail_write=false;
 fail_alloc=true;assert(sys_poll((uintptr_t)&fd,1,0,0,0,0)==(uint64_t)-12);fail_alloc=false;
 assert(sys_poll(1,1,0,0,0,0)==(uint64_t)-14);
 assert(sys_poll(0,UINT64_MAX,0,0,0,0)==(uint64_t)-22);
 struct {int64_t sec,nsec;} ts={0,1};
 assert(sys_ppoll((uintptr_t)&fd,1,(uintptr_t)&ts,0,0,0)==1 && seen_timeout==1);
 ts.sec=-1;assert(sys_ppoll(0,0,(uintptr_t)&ts,0,0,0)==(uint64_t)-22);
 ts.sec=0;ts.nsec=1000000000;assert(sys_ppoll(0,0,(uintptr_t)&ts,0,0,0)==(uint64_t)-22);
 ts.nsec=-1;assert(sys_ppoll(0,0,(uintptr_t)&ts,0,0,0)==(uint64_t)-22);
 ts.sec=INT64_MAX;ts.nsec=999999999;
 assert(sys_ppoll(0,0,(uintptr_t)&ts,0,0,0)==1 && seen_timeout==INT64_MAX);
 assert(sys_ppoll(0,0,UINT64_MAX,0,0,0)==(uint64_t)-14);
 assert(sys_ppoll(0,0,0,UINT64_MAX,8,0)==(uint64_t)-14);
 uint64_t mask=UINT64_MAX;
 assert(sys_ppoll(0,0,0,(uintptr_t)&mask,4,0)==(uint64_t)-22);
 answer=(uint64_t)-4;
 assert(sys_ppoll(0,0,0,(uintptr_t)&mask,8,0)==(uint64_t)-4);
 assert(seen_mask==(mask&~((1ULL<<8)|(1ULL<<18))) && self.signal_mask==123);
 assert(sys_ppoll(1,1,0,(uintptr_t)&mask,8,0)==(uint64_t)-14 && self.signal_mask==123);
 assert(!allocations);
 puts("PASS: poll snapshots, memory faults, bounds, timeout validation and signal-mask restoration");
}
'''
with tempfile.TemporaryDirectory(prefix='avory-poll-usercopy-') as td:
 p=Path(td);(p/'test.c').write_text(code)
 subprocess.run(['cc','-std=gnu11','-g','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-I',str(ROOT/'kernel/src/include'),str(p/'test.c'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True,env={**os.environ,'ASAN_OPTIONS':'detect_leaks=0'})
