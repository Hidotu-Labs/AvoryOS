#!/usr/bin/env python3
"""PID namespace ancestry, identities, wait/signal/proc, unshare and PID1 exit."""
from boot_probe import run_probe
INIT = r'''
#define _GNU_SOURCE
#include <errno.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <string.h>
#include <dirent.h>
#include <poll.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <sys/reboot.h>
#include <unistd.h>
static int failures;
#define CHECK(x) do { if(!(x)){printf("FAIL line=%d errno=%d: %s\n",__LINE__,errno,#x); failures++;} }while(0)
static void wait_ok(pid_t p) {int s;CHECK(waitpid(p,&s,0)==p && WIFEXITED(s) && WEXITSTATUS(s)==0);}
static ino_t identity(void) {struct stat s;return stat("/proc/self/ns/pid",&s)?0:s.st_ino;}
static void in_namespace(long outside, ino_t original) {
 CHECK(getpid()==1 && syscall(SYS_gettid)==1 && getppid()==0);
 CHECK(getpgrp()==1 && getsid(0)==1);
 CHECK(kill(1,SIGTERM)==0); /* Namespace init ignores unhandled signals. */
 CHECK(getpid()==1);
 CHECK(identity()!=0 && identity()!=original);
 CHECK(kill(outside,0)==-1 && errno==ESRCH);
 char path[64];struct stat s;snprintf(path,sizeof(path),"/proc/%ld",outside);
 CHECK(stat(path,&s)==-1 && errno==ENOENT);
 CHECK(stat("/proc/1/status",&s)==0);
 char link[64]={0};CHECK(readlink("/proc/self",link,sizeof(link)-1)==1 && !strcmp(link,"1"));
 pid_t p=fork();CHECK(p>0 || p==0);
 if(!p){CHECK(getpid()==2 && getppid()==1);_exit(failures?1:0);}
 if(p>0){CHECK(p==2);wait_ok(p);}
 p=fork();CHECK(p>=0);
 if(!p){for(;;)pause();}
 if(p>0){CHECK(kill(p,SIGKILL)==0);int st;CHECK(waitpid(p,&st,0)==p && WIFSIGNALED(st) && WTERMSIG(st)==SIGKILL);}
 p=syscall(SYS_clone,CLONE_NEWPID|SIGCHLD,0,0,0,0);CHECK(p>=0);
 if(!p){CHECK(getpid()==1 && getppid()==0);_exit(failures?1:0);}
 if(p>0)wait_ok(p);
 CHECK(unshare(CLONE_NEWPID)==0);CHECK(getpid()==1);
 p=fork();CHECK(p>=0);
 if(!p){CHECK(getpid()==1 && getppid()==0);_exit(failures?1:0);}
 if(p>0)wait_ok(p);
 CHECK(fork()==-1 && errno==ENOMEM); /* A dead namespace cannot admit children. */
 _exit(failures?1:0);
}
int main(void) {
 setvbuf(stdout,NULL,_IOLBF,0);puts("PID_NAMESPACE_BOOT: START");
 /* Leave the host PID1 process so outside has a noncolliding public number. */
 pid_t supervisor=fork();CHECK(supervisor>=0);
 if(!supervisor){
  long outside=getpid();ino_t original=identity();
  pid_t p=syscall(SYS_clone,CLONE_NEWPID|SIGCHLD,0,0,0,0);CHECK(p>=0);
  if(!p)in_namespace(outside,original);
  if(p>0)wait_ok(p);
  /* Namespace init death kills the otherwise sleeping orphan and closes
   * its inherited pipe writer. Keep an ancestor reader to observe cleanup. */
  int fds[2];CHECK(pipe(fds)==0);
  p=syscall(SYS_clone,CLONE_NEWPID|SIGCHLD,0,0,0,0);CHECK(p>=0);
  if(!p){
   close(fds[0]);pid_t orphan=fork();
   if(!orphan){for(;;)pause();}
   _exit(orphan<0?1:0);
  }
  close(fds[1]);if(p>0)wait_ok(p);
  struct pollfd pollfd={fds[0],POLLIN|POLLHUP,0};
  CHECK(poll(&pollfd,1,3000)>0);char byte;CHECK(read(fds[0],&byte,1)==0);close(fds[0]);
  _exit(failures?1:0);
 }
 if(supervisor>0)wait_ok(supervisor);
 errno=0;CHECK(syscall(SYS_clone,CLONE_NEWPID|CLONE_THREAD|CLONE_SIGHAND|CLONE_VM,0,0,0,0)==-1 && errno==EINVAL);
 printf("PID_NAMESPACE_BOOT: END failures=%d\n",failures);
 reboot(RB_POWER_OFF);return failures?1:0;
}
'''
run_probe('pid-namespace-boot', INIT, 'PID_NAMESPACE_BOOT: END failures=0')
