#!/usr/bin/env python3
"""Mount the real Forkit AppImage through its bundled libfuse/squashfuse server."""
import os
import shutil
from boot_probe import ROOT, run_probe
from test_appimage_exec_boot import setup as setup_runtime, prepare

INIT=r'''
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/wait.h>
#include <sys/reboot.h>
#include <unistd.h>
static void check(int ok,const char *what){
 if(ok)return;printf("APPIMAGE_FUSE_BOOT: FAIL %s errno=%d\n",what,errno);fflush(stdout);reboot(RB_POWER_OFF);for(;;)pause();
}
int main(void){
 setvbuf(stdout,NULL,_IOLBF,0);puts("APPIMAGE_FUSE_BOOT: START");
 int channel[2];check(!pipe(channel),"pipe");pid_t child=fork();check(child>=0,"fork runtime");
 if(!child){
  close(channel[0]);dup2(channel[1],1);close(channel[1]);
  char *args[]={"/release.AppImage","--appimage-mount",NULL};execv(args[0],args);_exit(127);
 }
 close(channel[1]);char mountpoint[512]={0};unsigned used=0;
 while(used<sizeof(mountpoint)-1){
  struct pollfd p={.fd=channel[0],.events=POLLIN};check(poll(&p,1,30000)>0,"mount announcement timeout");
  char c;check(read(channel[0],&c,1)==1,"mount announcement");if(c=='\n')break;mountpoint[used++]=c;
 }
 close(channel[0]);printf("APPIMAGE_FUSE_BOOT: mountpoint=%s\n",mountpoint);
 check(mountpoint[0]=='/',"absolute mountpoint");struct statfs fs;
 check(!statfs(mountpoint,&fs)&&fs.f_type==0x65735546,"native FUSE mount");
 char file[640];snprintf(file,sizeof(file),"%s/AppRun",mountpoint);
 struct stat st;check(!stat(file,&st),"AppRun lookup");
 int fd=open(file,O_RDONLY);check(fd>=0,"open bundled AppRun");
 unsigned char header[4];check(read(fd,header,4)==4,"read bundled AppRun");
 printf("APPIMAGE_FUSE_BOOT: AppRun first bytes=%02x %02x %02x %02x\n",header[0],header[1],header[2],header[3]);
 check(umount2(mountpoint,MNT_DETACH)==0,"detach native mount");
 check(lseek(fd,0,SEEK_SET)==0&&read(fd,header,4)==4,"open file survives lazy detach");
 close(fd);
 kill(child,SIGTERM);int status;check(waitpid(child,&status,0)==child,"runtime exit");
 puts("APPIMAGE_FUSE_BOOT: PASS");fflush(stdout);reboot(RB_POWER_OFF);for(;;)pause();
}
'''
def setup(root):
    setup_runtime(root)
    source=ROOT/'build/arch/rootfs/usr/lib/libfuse.so.2'
    shutil.copy2(source,root/'usr/lib/libfuse.so.2')

if __name__=='__main__':
    name=os.environ.get('AVORY_FUSE_TEST_NAME','appimage-fuse-boot')
    prefix=name.upper().replace('-','_')
    out=run_probe(name,INIT.replace('APPIMAGE_FUSE_BOOT',prefix),prefix+': PASS',
              setup=setup,prepare_disk=prepare,filesystem='ext4',disk_mib=384,
              cpus=2,timeout=120,accel=os.environ.get('AVORY_TEST_ACCEL','tcg'))

    log=(out/'serial.log').read_text(errors='replace')
    assert 'USER FAULT' not in log and 'SIGSEGV' not in log, 'runtime fault during mount/cleanup'
