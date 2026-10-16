#!/usr/bin/env python3
"""Enforced seccomp ABI, inheritance, TSYNC, TRAP and compat bypass regression."""
from pathlib import Path
import shutil
from boot_probe import run_probe

INIT = r'''
#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/reboot.h>
#include <sys/syscall.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <ucontext.h>
#include <unistd.h>
struct insn {uint16_t code;uint8_t jt,jf;uint32_t k;};
struct program {uint16_t len;struct insn *filter;};
#define I(c,k) {(c),0,0,(k)}
#define J(c,k,t,f) {(c),(t),(f),(k)}
#define ALLOW 0x7fff0000U
#define ERRNO 0x00050000U
#define TRAP 0x00030000U
#define KILL 0x80000000U
static int failures;
#define CHECK(x) do {if(!(x)){printf("FAIL line=%d errno=%d: %s\n",__LINE__,errno,#x);failures++;}}while(0)
static long install(struct insn *insns,unsigned count,unsigned flags) {
 struct program p={(uint16_t)count,insns};return syscall(SYS_seccomp,1,flags,&p);
}
static int deny(int nr,unsigned action,unsigned flags) {
 struct insn p[]={I(0x20,0),J(0x15,nr,0,1),I(6,action),I(6,ALLOW)};
 return install(p,4,flags);
}
static void verifier(void) {
 CHECK(prctl(PR_GET_SECCOMP)==0);
 CHECK(syscall(SYS_seccomp,1,1,NULL)==-1 && errno==EFAULT);
 struct insn badjump[]={I(5,99),I(6,ALLOW)};
 CHECK(install(badjump,2,0)==-1 && errno==EINVAL);
 struct insn divzero[]={I(0x34,0),I(6,ALLOW)};
 CHECK(install(divzero,2,0)==-1 && errno==EINVAL);
 struct insn oob[]={I(0x20,64),I(6,ALLOW)};
 CHECK(install(oob,2,0)==-1 && errno==EINVAL);
 struct insn mem[]={I(0x60,0),I(6,ALLOW)};
 CHECK(install(mem,2,0)==-1 && errno==EINVAL);
 struct insn join[]={I(0x20,0),J(0x15,39,1,0),I(2,0),I(0x60,0),I(6,ALLOW)};
 CHECK(install(join,5,0)==-1 && errno==EINVAL);
 CHECK(prctl(PR_GET_SECCOMP)==0);
 uint32_t action=ERRNO;CHECK(syscall(SYS_seccomp,2,0,&action)==0);
 action=0x7fc00000;CHECK(syscall(SYS_seccomp,2,0,&action)==-1 && errno==EOPNOTSUPP);
}
static void inherit(void) {
 CHECK(deny(SYS_getpid,ERRNO|123,0)==0);
 CHECK(prctl(PR_GET_SECCOMP)==2);
 CHECK(syscall(SYS_getpid)==-1 && errno==123);
 CHECK(syscall(SYS_getppid)>0);
 struct insn allow[]={I(6,ALLOW)};CHECK(install(allow,1,0)==0);
 CHECK(syscall(SYS_getpid)==-1 && errno==123); /* Cannot relax an older filter. */
 CHECK(deny(SYS_getpid,ERRNO|EACCES,0)==0);
 CHECK(syscall(SYS_getpid)==-1 && errno==EACCES); /* Newest same-action data wins. */
 CHECK(deny(SYS_getppid,ERRNO|EPERM,0)==0);
 CHECK(syscall(SYS_getppid)==-1 && errno==EPERM);
 pid_t child=fork();CHECK(child>=0);
 if(!child) {
  CHECK(syscall(SYS_getpid)==-1 && errno==EACCES);
  execl("/bin/probe-init","probe-init","filter-exec",NULL);_exit(99);
 }
 if(child>0){int st;CHECK(waitpid(child,&st,0)==child && WIFEXITED(st) && WEXITSTATUS(st)==0);}
}
static volatile int stage;
static long worker_tid;
static void *sync_worker(void *diverge) {
 worker_tid=syscall(SYS_gettid);
 if(diverge && deny(SYS_getppid,ERRNO|123,0)) return (void*)1;
 __atomic_store_n(&stage,1,__ATOMIC_RELEASE);
 while(__atomic_load_n(&stage,__ATOMIC_ACQUIRE)!=2)usleep(1000);
 if(diverge)return NULL;
 errno=0;return (void*)(uintptr_t)(syscall(SYS_getpid)!=-1 || errno!=EACCES || prctl(PR_GET_NO_NEW_PRIVS,0,0,0,0)!=1);
}
static void tsync(void) {
 stage=0;pthread_t t;void *result=(void*)99;
 CHECK(pthread_create(&t,NULL,sync_worker,NULL)==0);
 while(!__atomic_load_n(&stage,__ATOMIC_ACQUIRE))usleep(1000);
 CHECK(deny(SYS_getpid,ERRNO|EACCES,1)==0);
 CHECK(syscall(SYS_getpid)==-1 && errno==EACCES);
 __atomic_store_n(&stage,2,__ATOMIC_RELEASE);
 CHECK(pthread_join(t,&result)==0 && !result);
}
static void divergent(void) {
 stage=0;pthread_t t;void *result=(void*)99;
 CHECK(pthread_create(&t,NULL,sync_worker,(void*)1)==0);
 while(!__atomic_load_n(&stage,__ATOMIC_ACQUIRE))usleep(1000);
 CHECK(deny(SYS_getpid,ERRNO|EACCES,1)==worker_tid);
 CHECK(syscall(SYS_getpid)>0);CHECK(prctl(PR_GET_SECCOMP)==0);
 __atomic_store_n(&stage,2,__ATOMIC_RELEASE);
 CHECK(pthread_join(t,&result)==0 && !result);
}
static volatile sig_atomic_t trapped;
static void trap(int sig,siginfo_t *info,void *context) {
 if(sig==SIGSYS && info->si_code==1 && info->si_errno==42 &&
    info->si_syscall==SYS_getpid && info->si_arch==0xc000003e && info->si_call_addr)
  trapped=1;
 ((ucontext_t*)context)->uc_mcontext.gregs[REG_RAX]=777;
}
static void trap_test(void) {
 struct sigaction sa={.sa_sigaction=trap,.sa_flags=SA_SIGINFO};sigemptyset(&sa.sa_mask);
 CHECK(sigaction(SIGSYS,&sa,NULL)==0);
 sigset_t set;sigemptyset(&set);sigaddset(&set,SIGSYS);CHECK(pthread_sigmask(SIG_BLOCK,&set,NULL)==0);
 CHECK(deny(SYS_getpid,TRAP|42,0)==0);
 CHECK(syscall(SYS_getpid)==777 && trapped);
}
static void compat_kill(void) {
 struct insn arch[]={I(0x20,4),J(0x15,0xc000003e,1,0),I(6,KILL),I(6,ALLOW)};
 CHECK(install(arch,4,0)==0);
 CHECK(syscall(SYS_getpid)>0);
 __asm__ volatile("int $0x80" : : "a"(20) : "memory");
 _exit(99);
}
static void killed(void) {CHECK(deny(SYS_getpid,KILL,0)==0);syscall(SYS_getpid);_exit(99);}
static void privilege(void) {
 CHECK(setuid(1000)==0);
 CHECK(deny(SYS_getpid,ERRNO|EACCES,0)==-1 && errno==EACCES);
 CHECK(prctl(PR_SET_NO_NEW_PRIVS,1,0,0,0)==0);
 CHECK(deny(SYS_getpid,ERRNO|EACCES,0)==0);
 CHECK(syscall(SYS_getpid)==-1 && errno==EACCES);
}
static void setuid_exec(void) {
 CHECK(setuid(1000)==0);CHECK(prctl(PR_SET_NO_NEW_PRIVS,1,0,0,0)==0);
 execl("/bin/suid-probe","probe-init","nnp-exec",NULL);_exit(99);
}
static void run(void (*test)(void),int death) {
 pid_t child=fork();CHECK(child>=0);
 if(!child){failures=0;test();_exit(failures?1:0);}
 if(child>0){int st;CHECK(waitpid(child,&st,0)==child);
  if(death)CHECK(WIFSIGNALED(st) && WTERMSIG(st)==SIGSYS);
  else CHECK(WIFEXITED(st) && WEXITSTATUS(st)==0);}
}
int main(int argc,char **argv) {
 if(argc>1 && !strcmp(argv[1],"filter-exec"))
  return !(syscall(SYS_getpid)==-1 && errno==EACCES && prctl(PR_GET_SECCOMP)==2 && prctl(PR_GET_NO_NEW_PRIVS,0,0,0,0)==1);
 if(argc>1 && !strcmp(argv[1],"nnp-exec"))return !(getuid()==1000 && geteuid()==1000 && prctl(PR_GET_NO_NEW_PRIVS,0,0,0,0)==1);
 setvbuf(stdout,NULL,_IOLBF,0);puts("SECCOMP_BOOT: START");
 CHECK(chown("/bin/suid-probe",0,0)==0);CHECK(chmod("/bin/suid-probe",04755)==0);
 CHECK(prctl(PR_GET_NO_NEW_PRIVS,0,0,0,0)==0);
 run(privilege,0);run(setuid_exec,0);
 CHECK(prctl(PR_SET_NO_NEW_PRIVS,0,0,0,0)==-1 && errno==EINVAL);
 CHECK(prctl(PR_SET_NO_NEW_PRIVS,1,0,0,0)==0);
 CHECK(prctl(PR_GET_NO_NEW_PRIVS,0,0,0,0)==1);
 verifier();run(inherit,0);run(tsync,0);run(divergent,0);run(trap_test,0);run(killed,1);run(compat_kill,1);
 printf("SECCOMP_BOOT: END failures=%d\n",failures);fflush(NULL);
 reboot(RB_POWER_OFF);for(;;)sleep(1);
}
'''

def setup(root):
    target = root / 'bin/suid-probe'
    shutil.copy2(root / 'bin/probe-init', target)
    target.chmod(0o4755)

run_probe('seccomp-boot', INIT, 'SECCOMP_BOOT: END failures=0', setup=setup)
