#!/usr/bin/env python3
"""Exercise the actual syscall-user-dispatch configuration and entry filter."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'kernel/src/syscalls/sys_user_dispatch.c').read_text()
source = '\n'.join(line for line in source.splitlines() if not line.startswith('#include'))
abi = (root / 'kernel/src/syscalls/syscall.h').read_text()
a = abi.index('struct syscall_regs {')
b = abi.index('// Standard handler:', a)
prefix = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define SIGSYS 31
#define SIGSEGV 11
#define SIG_IGN 1
struct action { void *sa_handler; };
struct thread {
 bool syscall_dispatch_enabled, syscall_dispatch_sigsys;
 uint64_t syscall_dispatch_offset, syscall_dispatch_length;
 uint64_t syscall_dispatch_selector, syscall_dispatch_call_addr;
 int32_t syscall_dispatch_number;
 uint64_t signal_mask, pending_signals;
 uint32_t signal_sender_pid[64];
 struct action signal_handlers[64];
 int exit_status;
};
static bool is_user_range(void *p, unsigned long n) {
 uint64_t a=(uint64_t)p; return a<0x800000000000ULL && n<=0x800000000000ULL-a;
}
static bool unreadable;
static unsigned delivered, killed;
static int exited;
static uint8_t key;
static unsigned long copy_from_user(void *dst, void *src, unsigned long size) {
 assert(src==&key && size==1);if(unreadable)return size;memcpy(dst,src,size);return 0;
}
static void sched_terminate_thread_group(struct thread *t) { (void)t; killed++; }
static void process_do_exit(int status) { exited=status; }
'''
signal_stub = r'''
static uint64_t expected_rax;
static void signal_deliver_syscall(struct syscall_regs *regs) {
 assert(regs->rax==expected_rax); delivered++;
}
'''
tests = r'''
int main(void) {
 struct thread t={0};struct syscall_regs regs={.rax=12345,.rip=0x2000,.rdi=9};
 assert(!syscall_user_dispatch_intercept(&t,&regs));
 assert(syscall_user_dispatch_configure(&t,0,1,0,0)==-22);
 assert(syscall_user_dispatch_configure(&t,1,1,0,0)==-22);
 assert(syscall_user_dispatch_configure(&t,1,UINT64_MAX,2,0)==-22);
 assert(syscall_user_dispatch_configure(&t,3,0,0,0)==-22);
 assert(syscall_user_dispatch_configure(&t,1,0,0,UINT64_MAX)==-14);
 assert(!t.syscall_dispatch_enabled);
 assert(!syscall_user_dispatch_configure(&t,1,0x2000,10,(uint64_t)&key));
 /* Native interval is checked before touching an unreadable/invalid selector. */
 unreadable=true;key=2;
 assert(!syscall_user_dispatch_intercept(&t,&regs));
 regs.rip=0x2009;assert(!syscall_user_dispatch_intercept(&t,&regs));
 unreadable=false;key=0;regs.rip=0x2010;
 assert(!syscall_user_dispatch_intercept(&t,&regs));
 key=1;expected_rax=regs.rax;
 assert(syscall_user_dispatch_intercept(&t,&regs));
 assert(delivered==1 && t.syscall_dispatch_sigsys);
 assert(t.syscall_dispatch_number==12345 && t.syscall_dispatch_call_addr==0x2010);
 assert(t.pending_signals==(1ULL<<(SIGSYS-1)) && !killed);
 /* Failed configuration leaves the active configuration intact. */
 assert(syscall_user_dispatch_configure(&t,0,0,1,0)==-22);
 assert(t.syscall_dispatch_enabled && t.syscall_dispatch_offset==0x2000);
 key=2;assert(syscall_user_dispatch_intercept(&t,&regs));
 assert(killed==1 && exited==128+SIGSYS && delivered==1);
 unreadable=true;assert(syscall_user_dispatch_intercept(&t,&regs));
 assert(killed==2 && exited==128+SIGSEGV);
 unreadable=false;key=1;t.signal_mask=1ULL<<(SIGSYS-1);
 assert(syscall_user_dispatch_intercept(&t,&regs));assert(killed==3);
 t.signal_mask=0;t.signal_handlers[SIGSYS-1].sa_handler=(void *)SIG_IGN;
 assert(syscall_user_dispatch_intercept(&t,&regs));assert(killed==4);
 t.signal_handlers[SIGSYS-1].sa_handler=0;
 assert(!syscall_user_dispatch_configure(&t,1,0,0,0));
 assert(syscall_user_dispatch_intercept(&t,&regs));assert(delivered==2);
 assert(!syscall_user_dispatch_configure(&t,0,0,0,0));
 assert(!t.syscall_dispatch_enabled && !t.syscall_dispatch_sigsys);
 assert(!t.syscall_dispatch_selector && !t.syscall_dispatch_length);
 assert(!syscall_user_dispatch_intercept(&t,&regs));
 puts("PASS: dispatch configuration, native bounds, selector switching/faults, forced signals, metadata and OFF");
}
'''
with tempfile.TemporaryDirectory() as tmp:
    p = Path(tmp)
    (p / 'test.c').write_text(prefix + abi[a:b] + signal_stub + source + tests)
    subprocess.run(['cc', '-O2', '-Wall', '-Wextra', '-fsanitize=undefined',
                    str(p / 'test.c'), '-o', str(p / 'test')], check=True)
    subprocess.run([str(p / 'test')], check=True)
