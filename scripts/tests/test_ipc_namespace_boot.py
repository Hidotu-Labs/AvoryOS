#!/usr/bin/env python3
"""Real IPC namespace key/ID visibility, mappings, inheritance and reclamation."""
from boot_probe import run_probe

INIT = r'''
#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ipc.h>
#include <sys/reboot.h>
#include <sys/sem.h>
#include <sys/shm.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <unistd.h>
static int failures;
#define CHECK(x) do {if(!(x)){printf("FAIL line=%d errno=%d: %s\n",__LINE__,errno,#x);failures++;}}while(0)
static ino_t identity(const char *kind) {
 char path[128];struct stat st;
 snprintf(path,sizeof(path),"/proc/%ld/ns/%s",syscall(SYS_gettid),kind);
 return stat(path,&st)?0:st.st_ino;
}
static void wait_ok(pid_t pid) {
 int st;CHECK(waitpid(pid,&st,0)==pid && WIFEXITED(st) && WEXITSTATUS(st)==0);
}
static void *thread_share(void *arg) {
 int id=*(int*)arg;
 return (void*)(uintptr_t)(semctl(id,0,SETVAL,12)!=0);
}
int main(void) {
 setvbuf(stdout,NULL,_IOLBF,0);puts("IPC_NAMESPACE_BOOT: START");
 int shm=shmget(0x4242,4096,IPC_CREAT|0600),sem=semget(0x4242,1,IPC_CREAT|0600);
 CHECK(shm>=0 && sem>=0);CHECK(semctl(sem,0,SETVAL,3)==0);
 volatile int *shared=shmat(shm,NULL,0);CHECK(shared!=(void*)-1);*shared=7;
 ino_t original=identity("ipc"),uts=identity("uts");CHECK(original && uts);
 pid_t child=syscall(SYS_clone,CLONE_NEWIPC|CLONE_NEWUTS|SIGCHLD,0,0,0,0);CHECK(child>=0);
 if(!child) {
  CHECK(identity("ipc")!=original && identity("uts")!=uts);
  CHECK(shmget(0x4242,0,0)==-1 && errno==ENOENT);
  CHECK(semget(0x4242,0,0)==-1 && errno==ENOENT);
  CHECK(shmat(shm,NULL,0)==(void*)-1 && errno==EINVAL);
  CHECK(semctl(sem,0,GETVAL)==-1 && errno==EINVAL);
  struct shmid_ds ds;CHECK(shmctl(shm,IPC_STAT,&ds)==-1 && errno==EINVAL);
  CHECK(*shared==7);*shared=8; /* Existing mappings intentionally survive. */
  int local=shmget(0x4242,4096,IPC_CREAT|IPC_EXCL|0600);
  int local_sem=semget(0x4242,1,IPC_CREAT|IPC_EXCL|0600);
  CHECK(local>=0 && local!=shm && local_sem>=0 && local_sem!=sem);
  volatile int *mapping=shmat(local,NULL,0);CHECK(mapping!=(void*)-1);*mapping=99;
  CHECK(semctl(local_sem,0,SETVAL,11)==0);
  ino_t private_id=identity("ipc");
  pid_t grandchild=fork();CHECK(grandchild>=0);
  if(!grandchild) {
   CHECK(identity("ipc")==private_id && shmget(0x4242,0,0)==local);
   CHECK(semctl(local_sem,0,GETVAL)==11);
   _exit(failures?1:0);
  }
  if(grandchild>0)wait_ok(grandchild);
  pthread_t t;void *result=(void*)99;
  CHECK(pthread_create(&t,NULL,thread_share,&local_sem)==0);
  CHECK(pthread_join(t,&result)==0 && !result);
  CHECK(semctl(local_sem,0,GETVAL)==12);
  CHECK(unshare(CLONE_NEWIPC)==0 && identity("ipc")!=private_id);
  CHECK(shmget(0x4242,0,0)==-1 && errno==ENOENT);
  CHECK(semctl(local_sem,0,GETVAL)==-1 && errno==EINVAL);
  CHECK(*mapping==99);*mapping=101;CHECK(*mapping==101);CHECK(shmdt((void*)mapping)==0);
  CHECK(shmdt((void*)shared)==0);
  _exit(failures?1:0);
 }
 if(child>0)wait_ok(child);
 CHECK(identity("ipc")==original && identity("uts")==uts);
 CHECK(*shared==8 && shmget(0x4242,0,0)==shm);
 CHECK(semctl(sem,0,GETVAL)==3);
 /* Last namespace references must release unremoved objects and reusable slots. */
 for(int round=0;round<40;round++) {
  child=syscall(SYS_clone,CLONE_NEWIPC|SIGCHLD,0,0,0,0);CHECK(child>=0);
  if(!child){for(int j=0;j<8;j++)if(shmget(j+100,4096,IPC_CREAT|0600)<0 || semget(j+100,1,IPC_CREAT|0600)<0)_exit(1);_exit(0);}
  if(child>0)wait_ok(child);
 }
 CHECK(shmget(0x4242,0,0)==shm && semctl(sem,0,GETVAL)==3);
 CHECK(unshare(CLONE_NEWIPC|CLONE_NEWUSER)==-1 && errno==EPERM);
 CHECK(identity("ipc")==original);
 CHECK(syscall(SYS_clone,CLONE_NEWIPC|CLONE_SYSVSEM|SIGCHLD,0,0,0,0)==-1 && errno==EINVAL);
 child=fork();CHECK(child>=0);
 if(!child) {if(setuid(1000))_exit(1);if(unshare(CLONE_NEWIPC)!=-1 || errno!=EPERM)_exit(2);_exit(0);}
 if(child>0)wait_ok(child);
 CHECK(shmctl(shm,IPC_RMID,NULL)==0);CHECK(shmdt((void*)shared)==0);
 CHECK(semctl(sem,0,IPC_RMID)==0);
 printf("IPC_NAMESPACE_BOOT: END failures=%d\n",failures);fflush(NULL);
 reboot(RB_POWER_OFF);for(;;)sleep(1);
}
'''
run_probe('ipc-namespace-boot', INIT, 'IPC_NAMESPACE_BOOT: END failures=0')
