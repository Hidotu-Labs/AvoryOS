#!/usr/bin/env python3
"""Exercise real journal ownership with competing nice levels on one CPU."""
from boot_probe import run_probe
INIT=r'''
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/reboot.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
static int failures;
#define CHECK(x) do{if(!(x)){printf("FAIL line=%d errno=%d: %s\n",__LINE__,errno,#x);failures++;}}while(0)
static uint64_t now(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return (uint64_t)t.tv_sec*1000000000+t.tv_nsec;}
static void writer(int nice,int id){
 CHECK(setpriority(PRIO_PROCESS,0,nice)==0);
 char a[80],b[80],data[4096];memset(data,0x41+id,sizeof(data));
 uint64_t start=now();
 for(int i=0;i<32;i++){
  snprintf(a,sizeof(a),"/root/priority-%d-%d",id,i);snprintf(b,sizeof(b),"%s.new",a);
  int fd=open(a,O_RDWR|O_CREAT|O_EXCL,0600);CHECK(fd>=0);
  if(fd>=0){CHECK(write(fd,data,sizeof(data))==sizeof(data));CHECK(fsync(fd)==0);CHECK(close(fd)==0);}
  CHECK(rename(a,b)==0);CHECK(unlink(b)==0);
  CHECK(syscall(SYS_getpriority,PRIO_PROCESS,0)==20-nice);
 }
 printf("JOURNAL_PRIORITY_BOOT: writer=%d nice=%d elapsed_ms=%llu\n",id,nice,(unsigned long long)((now()-start)/1000000));
 _exit(failures?1:0);
}
int main(void){
 setvbuf(stdout,NULL,_IOLBF,0);puts("JOURNAL_PRIORITY_BOOT: START");
 pid_t hog=fork();CHECK(hog>=0);
 if(!hog){CHECK(setpriority(PRIO_PROCESS,0,-20)==0);uint64_t finish=now()+5000000000ULL;while(now()<finish){uint64_t until=now()+10000000;while(now()<until){}usleep(1000);}_exit(failures?1:0);}
 pid_t bg=fork();CHECK(bg>=0);if(!bg)writer(19,1);
 pid_t fg=fork();CHECK(fg>=0);if(!fg)writer(0,2);
 int status;
 if(bg>0)CHECK(waitpid(bg,&status,0)==bg&&WIFEXITED(status)&&!WEXITSTATUS(status));
 if(fg>0)CHECK(waitpid(fg,&status,0)==fg&&WIFEXITED(status)&&!WEXITSTATUS(status));
 if(hog>0)CHECK(waitpid(hog,&status,0)==hog&&WIFEXITED(status)&&!WEXITSTATUS(status));
 printf("JOURNAL_PRIORITY_BOOT: END failures=%d\n",failures);fflush(NULL);reboot(RB_POWER_OFF);return failures?1:0;
}
'''
if __name__=='__main__':
    run_probe('journal-priority-boot',INIT,'JOURNAL_PRIORITY_BOOT: END failures=0',cpus=1,timeout=90,filesystem='ext3')
