#!/usr/bin/env python3
"""Compile actual epoll pwait syscall wrappers against a controlled wait backend."""
from pathlib import Path
import subprocess
import tempfile
ROOT=Path(__file__).resolve().parents[2]
s=(ROOT/'kernel/src/syscalls/sys_epoll.c').read_text()
code=s[s.index('/* Shared pwait path:'):s.index('// Register epoll syscalls')]
prefix=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define SIGKILL 9
#define SIGSTOP 19
typedef struct { int unused; } eventpoll_t;
struct epoll_event { uint32_t events; uint64_t data; };
struct thread { uint64_t signal_mask; };
static struct thread thread={.signal_mask=123};
static eventpoll_t ep;
static bool copy_failure;
static int calls, wait_return;
static int64_t last_timeout;
static uint64_t last_mask;
static eventpoll_t *epoll_from_fd(int fd) { return fd==7?&ep:NULL; }
static bool epoll_user_ptr(uint64_t p) { return p && p<0x800000000000ull; }
static struct thread *sched_get_current(void) { return &thread; }
static unsigned long copy_from_user(void *to,const void *from,unsigned long n) {
 if(copy_failure || (uintptr_t)from==1 || n>0x800000000000ull-(uintptr_t)from) return n;
 memcpy(to,from,n);return 0;
}
static int epoll_wait_impl(eventpoll_t *p,struct epoll_event *e,int max,int64_t timeout) {
 assert(p==&ep && e && max>0);calls++;last_timeout=timeout;last_mask=thread.signal_mask;return wait_return;
}
'''
tests=r'''
#define PTR(p) ((uint64_t)(uintptr_t)(p))
int main(void) {
 struct epoll_event events[2];struct { int64_t sec,nsec; } ts={0,1};
 assert(sys_epoll_pwait2(7,PTR(events),2,0,0,0)==0 && last_timeout==-1);
 assert(sys_epoll_pwait2(7,PTR(events),2,PTR(&ts),0,0)==0 && last_timeout==1);
 ts.nsec=1000000;sys_epoll_pwait2(7,PTR(events),2,PTR(&ts),0,0);assert(last_timeout==1);
 ts.sec=3;ts.nsec=1000001;sys_epoll_pwait2(7,PTR(events),2,PTR(&ts),0,0);assert(last_timeout==3002);
 ts.sec=INT64_MAX;ts.nsec=999999999;sys_epoll_pwait2(7,PTR(events),2,PTR(&ts),0,0);assert(last_timeout==INT64_MAX);
 ts.sec=0;ts.nsec=0;sys_epoll_pwait2(7,PTR(events),2,PTR(&ts),0,0);assert(last_timeout==0);
 int before=calls;
 ts.sec=-1;assert((int64_t)sys_epoll_pwait2(7,PTR(events),2,PTR(&ts),0,0)==-22);
 ts.sec=0;ts.nsec=-1;assert((int64_t)sys_epoll_pwait2(7,PTR(events),2,PTR(&ts),0,0)==-22);
 ts.nsec=1000000000;assert((int64_t)sys_epoll_pwait2(7,PTR(events),2,PTR(&ts),0,0)==-22);
 ts.nsec=0;copy_failure=true;assert((int64_t)sys_epoll_pwait2(7,PTR(events),2,PTR(&ts),0,0)==-14);
 copy_failure=false;assert((int64_t)sys_epoll_pwait2(7,PTR(events),2,1,0,0)==-14);
 assert((int64_t)sys_epoll_pwait2(9,PTR(events),2,0,0,0)==-9);
 assert((int64_t)sys_epoll_pwait2(7,0,2,0,0,0)==-14);
 assert((int64_t)sys_epoll_pwait2(7,PTR(events),0,0,0,0)==-22);
 uint64_t mask=UINT64_MAX;
 assert((int64_t)sys_epoll_pwait2(7,PTR(events),2,0,PTR(&mask),128)==-22);
 assert(calls==before && thread.signal_mask==123);
 wait_return=-4;
 assert((int64_t)sys_epoll_pwait2(7,PTR(events),2,0,PTR(&mask),8)==-4);
 assert(last_mask==(mask&~((1ull<<8)|(1ull<<18))) && thread.signal_mask==123);
 copy_failure=true;assert((int64_t)sys_epoll_pwait(7,PTR(events),2,0,PTR(&mask),8)==-14);
 assert(thread.signal_mask==123);copy_failure=false;
 sys_epoll_pwait(7,PTR(events),2,(uint64_t)-2,0,0);assert(last_timeout==-1);
 sys_epoll_pwait(7,PTR(events),2,INT32_MAX,0,0);assert(last_timeout==INT32_MAX);
 puts("PASS: pwait2 relative timespec rounding/saturation, poll/infinite, invalid inputs, signal set size and mask restoration");
}
'''
with tempfile.TemporaryDirectory() as tmp:
    p=Path(tmp);(p/'test.c').write_text(prefix+code+tests)
    subprocess.run(['cc','-std=gnu11','-Wall','-Wextra','-Werror','-fsanitize=undefined',str(p/'test.c'),'-o',str(p/'test')],check=True)
    subprocess.run([str(p/'test')],check=True)
