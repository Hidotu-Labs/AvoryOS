#!/usr/bin/env python3
"""Relative exec, procfs reopen after chdir, and self-reexec on a private disk."""
import os
import subprocess
from boot_probe import run_probe

INIT=r'''
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/reboot.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
static void check(int ok,const char *what){
 if(ok)return;printf("PROC_SELF_EXE_BOOT: FAIL %s errno=%d\n",what,errno);fflush(stdout);_exit(1);
}
int main(int argc,char **argv){
 setvbuf(stdout,NULL,_IOLBF,0);
 if(argc>1){
  char name[256]={0},after[256]={0};
  ssize_t len=readlink("/proc/self/exe",name,sizeof(name)-1);
  check(len>0 && name[0]=='/',"absolute proc link");
  check(!chdir("/"),"change directory after exec");
  int fd=open("/proc/self/exe",O_RDONLY);check(fd>=0,"reopen executable after chdir");
  struct stat a,b;check(!fstat(fd,&a)&&!stat("/bin/probe-init",&b)&&a.st_ino==b.st_ino&&a.st_size==b.st_size,"same executable inode");
  char magic[4];check(read(fd,magic,4)==4&&!memcmp(magic,"\177ELF",4),"ELF bytes through proc");close(fd);
  if(!strcmp(argv[1],"relative")){
   char *missing[]={"/not-an-executable",NULL};
   execv(missing[0],missing);check(errno==ENOENT,"failed exec error");
   check(readlink("/proc/self/exe",after,sizeof(after)-1)==len&&!strcmp(name,after),"failed exec preserves proc link");
   char *next[]={"/proc/self/exe","alias",NULL};execv(next[0],next);
   check(0,"self reexec after chdir");
  }
  return 0;
 }
 puts("PROC_SELF_EXE_BOOT: START");pid_t pid=fork();
 if(!pid){
  check(!chdir("/bin"),"set launch cwd");
  char *next[]={"./probe-init","relative",NULL};execv(next[0],next);check(0,"relative exec");
 }
 int status=-1;
 if(pid>0&&waitpid(pid,&status,0)==pid&&WIFEXITED(status)&&!WEXITSTATUS(status))
  puts("PROC_SELF_EXE_BOOT: PASS");
 else printf("PROC_SELF_EXE_BOOT: FAIL status=%d\n",status);
 fflush(stdout);reboot(RB_POWER_OFF);for(;;)pause();
}
'''
def prepare(disk,out):
    subprocess.run(['mke2fs','-q','-F','-t','ext2','-O','none,filetype,sparse_super','-d',str(out/'root'),str(disk)],check=True)

if __name__=='__main__':
    run_probe('proc-self-exe-boot',INIT,'PROC_SELF_EXE_BOOT: PASS',
              prepare_disk=prepare,cpus=2,timeout=90,
              accel=os.environ.get('AVORY_TEST_ACCEL','tcg'))
