#!/usr/bin/env python3
"""Exercise exit's actual user accesses, including assembly exception fixups."""
from pathlib import Path
import subprocess
import tempfile
root = Path(__file__).resolve().parents[2]
s = (root/'kernel/src/syscalls/sys_process.c').read_text()
clear = s[s.index('static void process_clear_child_tid('):s.index('\nvoid process_do_exit(uint64_t status) {')]
robust = s[s.index('static void robust_handle_futex_death('):s.index('\nstatic void robust_handle_death(struct thread *t) {')]
c = r'''
#define _GNU_SOURCE
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include <signal.h>
#include <ucontext.h>
uint8_t cpu_has_smap_flag;
struct thread { void *tid_address; };
struct entry { uintptr_t insn, fixup; };
extern struct entry __start___ex_table[], __stop___ex_table[];
extern int cmpxchg_user_u32(uint32_t *,uint32_t *,uint32_t);
extern unsigned long copy_to_user(void *,const void *,unsigned long);
static unsigned wakes;
static bool writable=true;
static bool vmm_is_user_addr_range_writable(uint64_t a,size_t n) {
 (void)a;(void)n;return writable;
}
static uint64_t futex_wake_user(uint32_t *a,uint32_t n) {
 (void)a;assert(n==1);wakes++;return 1;
}
static unsigned long copy_from_user(void *,const void *,unsigned long);
static void fault(int sig,siginfo_t *info,void *ctx) {
 (void)sig;(void)info;ucontext_t *u=ctx;
 for(struct entry *e=__start___ex_table;e<__stop___ex_table;e++) {
  if(e->insn==(uintptr_t)u->uc_mcontext.gregs[REG_RIP]) {
   u->uc_mcontext.gregs[REG_RIP]=e->fixup;return;
  }
 }
 assert(!"unguarded user access");
}
'''.replace('static unsigned long copy_from_user','extern unsigned long copy_from_user')
tests = r'''
int main(void) {
 struct sigaction sa={.sa_sigaction=fault,.sa_flags=SA_SIGINFO};
 sigemptyset(&sa.sa_mask);sigaction(SIGSEGV,&sa,NULL);
 uint32_t value=123,expected=123;
 assert(cmpxchg_user_u32(&value,&expected,456)==1 && value==456);
 expected=123;assert(cmpxchg_user_u32(&value,&expected,789)==0);
 assert(expected==456 && value==456);
 assert(cmpxchg_user_u32((void *)0x800000000000ULL,&expected,0)==-14);
 assert(cmpxchg_user_u32((void *)UINTPTR_MAX,&expected,0)==-14);
 assert(cmpxchg_user_u32((void *)4096,&expected,0)==-14);
 struct thread t={.tid_address=&value};process_clear_child_tid(&t);
 assert(value==0 && wakes==1 && t.tid_address==NULL);
 t.tid_address=(void *)4096;process_clear_child_tid(&t);
 assert(wakes==1 && t.tid_address==NULL);
 value=42;writable=false;t.tid_address=&value;process_clear_child_tid(&t);
 assert(value==42 && wakes==1 && t.tid_address==NULL);
 writable=true;value=0x8000002a;robust_handle_futex_death((uintptr_t)&value,42);
 assert(value==0xc0000000 && wakes==2);
 value=43;robust_handle_futex_death((uintptr_t)&value,42);
 assert(value==43 && wakes==2);
 robust_handle_futex_death(4096,42);assert(wakes==2);
 puts("PASS: clear_child_tid and robust futex success, ownership mismatch, bad pointers and assembly fault recovery");
}
'''
with tempfile.TemporaryDirectory() as tmp:
 p=Path(tmp);(p/'test.c').write_text(c+clear+robust+tests)
 subprocess.run(['nasm','-f','elf64',str(root/'kernel/src/arch/x86_64/uaccess.asm'),'-o',str(p/'uaccess.o')],check=True)
 subprocess.run(['cc','-no-pie','-Wall','-Wextra','-Werror',str(p/'test.c'),str(p/'uaccess.o'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
