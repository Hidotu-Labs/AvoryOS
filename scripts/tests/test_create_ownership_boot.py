#!/usr/bin/env python3
"""Directory observers must never see root ownership during user creation."""
import subprocess
from boot_probe import run_probe
INIT = r'''
#define _GNU_SOURCE
#include <sys/inotify.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/reboot.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
static void handler(int s){(void)s;}
int main(void){
 setbuf(stdout,NULL);puts("CREATE_OWNERSHIP_BOOT: START");signal(SIGCHLD,handler);
 mkdir("/watch",0777);chown("/watch",0,2000);chmod("/watch",02777);
 int fd=inotify_init1(0);if(fd<0 || inotify_add_watch(fd,"/watch",IN_CREATE)<0)goto fail;
 pid_t child=fork();if(child<0)goto fail;
 if(!child){setgid(1000);setuid(1000);umask(077);
  for(int i=0;i<1000;i++){char path[128];sprintf(path,"/watch/d%d",i);
   if(mkdir(path,0775))_exit(2);
   sprintf(path,"/watch/d%d/file",i);int f=open(path,O_CREAT|O_WRONLY,0644);
   if(f<0)_exit(3);close(f);
  }_exit(0);
 }
 int count=0,bad=0;char events[8192] __attribute__((aligned(8)));
 while(count<1000){int n=read(fd,events,sizeof(events));if(n<=0)goto fail;
  for(int off=0;off<n;){struct inotify_event *e=(void *)(events+off);
   char path[256];sprintf(path,"/watch/%s",e->name);struct stat st;
   if(stat(path,&st) || st.st_uid!=1000 || st.st_gid!=2000 || !(st.st_mode&S_ISGID)){
    printf("CREATE_OWNERSHIP_BOOT: bad owner %s uid=%u gid=%u mode=%o\n",path,st.st_uid,st.st_gid,st.st_mode);bad++;
   }count++;off+=sizeof(*e)+e->len;
  }
 }
 int status;if(waitpid(child,&status,0)!=child || status || bad)goto fail;
 puts("CREATE_OWNERSHIP_BOOT: PASS");reboot(RB_POWER_OFF);for(;;)pause();
 fail:puts("CREATE_OWNERSHIP_BOOT: FAIL");reboot(RB_POWER_OFF);for(;;)pause();
}
'''
def prepare(disk,out):
    subprocess.run(['mke2fs','-q','-F','-t','ext4','-b','1024','-I','128',
                    '-O','none,filetype,sparse_super,extent,has_journal',
                    '-d',str(out/'root'),str(disk)],check=True)
if __name__=='__main__':
    run_probe('create-ownership-boot',INIT,'CREATE_OWNERSHIP_BOOT: PASS',
              prepare_disk=prepare,accel='tcg',cpus=2,timeout=90,disk_mib=128)
