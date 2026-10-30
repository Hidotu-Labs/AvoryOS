#!/usr/bin/env python3
"""Run real signal-frame construction with real uaccess exception fixups.

Read-only backing with a writable VMA models failed CoW resolution. Cover a
partial frame copy and a separately failing restorer store as well as ABI
pointers, red-zone preservation, queued siginfo, masks and alternate stacks.
"""
from pathlib import Path
import subprocess
import tempfile
import sys
ROOT = Path(__file__).resolve().parents[2]
s = Path(sys.argv[1]).read_text() if len(sys.argv)>1 else (ROOT/'kernel/src/syscalls/sys_signal.c').read_text()
def function(sig):
 start=s.index(sig);end=s.index('{',start)+1;depth=1
 while depth:
  depth+=(s[end]=='{')-(s[end]=='}');end+=1
 return s[start:end]+'\n'
structs=s[s.index('struct kernel_siginfo {'):s.index('/* Keep complete x86-64 siginfo')]
structs+=s[s.index('struct kernel_sigaltstack {'):s.index('_Static_assert(sizeof(struct kernel_siginfo)')]
c=r'''
#define _GNU_SOURCE
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <setjmp.h>
#include <signal.h>
#include <ucontext.h>
#include <sys/mman.h>
#include "kernel/src/cpu/isr.h"
#define SA_RESTORER 0x04000000
#undef si_pid
#undef si_uid
#undef si_addr
#undef si_call_addr
#undef si_syscall
#undef si_arch
#undef sa_handler
#define X86_64_RED_ZONE_SIZE 128
#define UC_SIGCONTEXT_SS 2
#define UC_STRICT_RESTORE_SS 4
'''+structs+r'''
struct k_sigaction {void *sa_handler; uint64_t sa_flags;void *sa_restorer;uint64_t sa_mask;};
struct thread {
 uint64_t pending_signals,signal_mask,thread_directed_signals;
 bool syscall_dispatch_sigsys,fault_regs_valid,has_saved_signal_mask;
 struct registers fault_regs;
 struct k_sigaction signal_handlers[64];
 uint64_t fault_addr,fault_code,saved_signal_mask,ss_sp,ss_size;
 int ss_flags,exit_status,tid,last_report_sig;
 char comm[16];
 uint32_t signal_sender_pid[64];int signal_sender_code[64];
 uint64_t syscall_dispatch_call_addr;int syscall_dispatch_number;
 bool seccomp_sigsys;uint16_t seccomp_trap_data;uint32_t seccomp_trap_arch;
 uint8_t termination_signal;
 struct mm {int lock,vmas;} *mm;
 uint8_t fpu_state[4096] __attribute__((aligned(64)));
};
static struct thread task;
typedef int spinlock_t;
static spinlock_t tid_lock;
static struct thread *sched_get_current(void){return &task;}
static uint32_t pid_namespace_visible(struct thread *t,uint32_t pid){(void)t;return pid;}
static void spinlock_acquire(int *l){assert(!*l);*l=1;}
static void spinlock_release(int *l){assert(*l);*l=0;}
static bool spinlock_try_acquire(int *l){spinlock_acquire(l);return true;}
struct vma {int unused;};
static struct vma *vma_find(int *v,uint64_t a){(void)v;(void)a;return NULL;}
static bool queued;
static bool signal_dequeue_locked(struct thread *t,uint64_t b,struct kernel_siginfo *i){
 t->pending_signals&=~b;
 if(queued){memset(i,0,sizeof(*i));i->si_signo=SIGUSR1;i->si_code=-1;i->_kill.si_pid=42;}return queued;
}
static void syscall_restart_return(struct registers *r,uint64_t n,uint64_t ip,bool h,uint64_t f){(void)r;(void)n;(void)ip;(void)h;(void)f;}
static bool signal_default_is_ignore(int n){(void)n;return false;}
static void test_log(const char *format,...){(void)format;}
#define klogf test_log
static void klog_puts(const char *s){(void)s;}
static void klog_uint64(uint64_t n){(void)n;}
static int process_core_dump_enabled;
static void process_dump_core(struct thread *t,struct registers *r,int sig){(void)t;(void)r;(void)sig;}
static unsigned groups_terminated;
static void sched_terminate_thread_group(struct thread *t){assert(t->exit_status==139);groups_terminated++;}
void isr_report_user_fault(struct registers *r,int s,uint64_t a){(void)r;(void)s;(void)a;assert(0);}
static jmp_buf terminated;
static int exit_status;
static void process_do_exit(int code){exit_status=code;longjmp(terminated,1);}
static bool vmm_is_user_addr_range_writable(uint64_t a,size_t n){(void)a;(void)n;return true;}
static bool on_sig_stack(struct thread *t,uint64_t sp){return sp>=t->ss_sp && sp-t->ss_sp<t->ss_size;}
static bool replace_action;
static void fpu_ensure_loaded(struct thread *t){
 if(replace_action){t->signal_handlers[SIGUSR1-1]=(struct k_sigaction){0};replace_action=false;}
}
static bool cpu_has_xsave_flag;
static uint64_t cpu_xsave_mask;
uint8_t cpu_has_smap_flag;
extern unsigned long copy_to_user(void *,const void *,unsigned long);
struct entry {uintptr_t insn,fixup;};
extern struct entry __start___ex_table[],__stop___ex_table[];
static unsigned recovered;
static void fault(int sig,siginfo_t *info,void *ctx){
 (void)sig;(void)info;ucontext_t *u=ctx;
 for(struct entry *e=__start___ex_table;e<__stop___ex_table;e++)
  if(e->insn==(uintptr_t)u->uc_mcontext.gregs[REG_RIP]){u->uc_mcontext.gregs[REG_RIP]=e->fixup;recovered++;return;}
 fputs("FAIL: unguarded signal-frame user access\n",stderr);_Exit(90);
}
static void signal_compat_deliver(struct registers *r,int sig,struct thread *t,
 struct k_sigaction *sa,const struct kernel_siginfo *info){
 (void)r;(void)sig;(void)t;(void)sa;(void)info;assert(0);
}
static void ptrace_user_return_stop(struct registers *r){(void)r;}
static int ptrace_signal_delivery_stop(struct registers *r,int sig,
 const struct kernel_siginfo *info){(void)r;(void)sig;(void)info;return -1;}
'''+function('static void fill_signal_context(')+function('static void signal_frame_terminate(')+function('static void signal_deliver_frame(')+r'''
static struct registers regs;
static void reset(uint8_t *frame){
 memset(&task,0,sizeof(task));queued=false;exit_status=0;replace_action=false;
 task.pending_signals=1ULL<<(SIGUSR1-1);task.tid=123;
 task.signal_handlers[SIGUSR1-1]=(struct k_sigaction){.sa_handler=(void*)0x123450,.sa_flags=SA_RESTORER|SA_SIGINFO,.sa_restorer=(void*)0x543210,.sa_mask=4};
 regs=(struct registers){.cs=0x2b,.ss=0x23,.rsp=(uintptr_t)frame+sizeof(struct sigframe)+128,.rip=0x8888,.rax=999};
}
int main(void){
 struct sigaction sa={.sa_sigaction=fault,.sa_flags=SA_SIGINFO};sigemptyset(&sa.sa_mask);assert(!sigaction(SIGSEGV,&sa,NULL));
 uint8_t *mem=mmap(NULL,16384,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);assert(mem!=MAP_FAILED);
 uint8_t *dest=mem+4096;
 reset(dest);memset(mem,0xa5,16384);queued=true;task.has_saved_signal_mask=true;task.saved_signal_mask=8;
 signal_deliver_frame(&regs,UINT64_MAX,0);
 struct sigframe *f=(void*)dest;
 assert(f->regs.rip==0x8888 && f->regs.rax==999 && f->mask==8);
 assert(f->info.si_code==-1 && f->info._kill.si_pid==42);
 assert(f->ucontext.uc_mcontext.fpregs==(uintptr_t)&f->ucontext.fpregs_mem[0]);
 assert(f->ucontext.uc_sigmask[0]==8);
 assert(regs.rsi==(uintptr_t)&f->info && regs.rdx==(uintptr_t)&f->ucontext);
 assert(regs.rsp==(uintptr_t)dest-8 && *(uint64_t*)regs.rsp==0x543210);
 assert(regs.rip==0x123450 && regs.rdi==SIGUSR1 && !task.has_saved_signal_mask);
 for(unsigned i=0;i<128;i++)assert(dest[sizeof(*f)+i]==0xa5);
 assert(task.signal_mask==(4|(1ULL<<(SIGUSR1-1))));
 /* Simulate a sibling resetting the disposition after handler selection. */
 reset(dest);replace_action=true;signal_deliver_frame(&regs,UINT64_MAX,0);
 assert(task.signal_handlers[SIGUSR1-1].sa_handler==NULL);
 assert(regs.rip==0x123450 && *(uint64_t*)regs.rsp==0x543210);
 assert(task.signal_mask==(4|(1ULL<<(SIGUSR1-1))));
 /* The VMA remains writable, but the first CoW write cannot be resolved. */
 reset(dest);assert(!mprotect(dest,4096,PROT_READ));
 if(!setjmp(terminated)){signal_deliver_frame(&regs,UINT64_MAX,0);assert(0);}
 assert(exit_status==139 && recovered==1 && groups_terminated==1);assert(!mprotect(dest,4096,PROT_READ|PROT_WRITE));
 /* A later frame page fails after some bytes were already copied. */
 reset(mem+8192-16);assert(!mprotect(mem+8192,4096,PROT_READ));
 if(!setjmp(terminated)){signal_deliver_frame(&regs,UINT64_MAX,0);assert(0);}
 assert(exit_status==139 && recovered==2 && groups_terminated==2);assert(!mprotect(mem+8192,4096,PROT_READ|PROT_WRITE));
 /* Frame succeeds, return address on the preceding page fails. */
 reset(dest);assert(!mprotect(mem,4096,PROT_READ));
 if(!setjmp(terminated)){signal_deliver_frame(&regs,UINT64_MAX,0);assert(0);}
 assert(exit_status==139 && recovered==3 && groups_terminated==3);assert(!mprotect(mem,4096,PROT_READ|PROT_WRITE));
 /* Alternate-stack arguments and fpregs must still point into user memory. */
 reset(dest);task.ss_sp=(uintptr_t)dest;task.ss_size=8192;task.signal_handlers[SIGUSR1-1].sa_flags|=SA_ONSTACK;regs.rsp=(uintptr_t)mem+1024;
 signal_deliver_frame(&regs,UINT64_MAX,0);
 f=(void*)(regs.rsp+8);assert((uintptr_t)f>=task.ss_sp);
 assert(regs.rdx==(uintptr_t)&f->ucontext && f->ucontext.uc_mcontext.fpregs==(uintptr_t)f->ucontext.fpregs_mem);
 munmap(mem,16384);puts("PASS: signal-frame ABI, red zone, alternate stack, and guarded initial/partial/restorer fault recovery");
}
'''
with tempfile.TemporaryDirectory(prefix='avory-signal-frame-') as tmp:
 p=Path(tmp);(p/'test.c').write_text(c)
 subprocess.run(['nasm','-f','elf64',str(ROOT/'kernel/src/arch/x86_64/uaccess.asm'),'-o',str(p/'uaccess.o')],check=True)
 subprocess.run(['cc','-no-pie','-O2','-Wall','-Wextra','-Werror','-I',str(ROOT),str(p/'test.c'),str(p/'uaccess.o'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
