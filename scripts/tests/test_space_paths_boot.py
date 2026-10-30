#!/usr/bin/env python3
"""Filesystem paths must preserve spaces in GOG-style directory names."""
import subprocess
from boot_probe import run_probe
INIT = r'''
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/reboot.h>
#include <unistd.h>
static void done(int ok,const char *why){printf("SPACE_PATHS_BOOT: %s %s\n",ok?"PASS":"FAIL",why);reboot(RB_POWER_OFF);for(;;)pause();}
int main(void){setbuf(stdout,NULL);puts("SPACE_PATHS_BOOT: START");
 const char *dir="/home/GOG Games/Terraria Game";
 if(mkdir("/home",0755)&&errno!=EEXIST)done(0,"mkdir home");
 if(mkdir("/home/GOG Games",0755))done(0,"mkdir GOG Games");
 if(mkdir(dir,0755))done(0,"mkdir Terraria Game");
 char path[256];snprintf(path,sizeof(path),"%s/start game.sh",dir);
 int fd=open(path,O_CREAT|O_WRONLY|O_TRUNC,0755);if(fd<0)done(0,"create spaced filename");
 const char body[]="#!/bin/sh\necho GOG_PATH_OK\n";
 if(write(fd,body,sizeof(body)-1)!=(ssize_t)sizeof(body)-1)done(0,"write spaced filename");close(fd);
 struct stat st;if(stat(path,&st)||st.st_size!=(off_t)sizeof(body)-1)done(0,"stat spaced path");
 fd=open(path,O_RDONLY);char got[64]={0};if(fd<0||read(fd,got,sizeof(got))!=(ssize_t)sizeof(body)-1||memcmp(got,body,sizeof(body)-1))done(0,"read spaced path");close(fd);
 if(rename(path,"/home/GOG Games/Terraria Game/renamed game.sh"))done(0,"rename spaced path");
 if(access("/home/GOG Games/Terraria Game/renamed game.sh",X_OK))done(0,"access spaced path");
 done(1,"mkdir, create, read, rename, access");
}
'''
def prepare(disk,out):
 subprocess.run(['mke2fs','-q','-F','-t','ext4','-b','1024','-I','128','-O','none,filetype,sparse_super,extent,has_journal','-d',str(out/'root'),str(disk)],check=True)
if __name__=='__main__':
 run_probe('space-paths-boot',INIT,'SPACE_PATHS_BOOT: PASS',prepare_disk=prepare,accel='tcg',cpus=1,timeout=90,disk_mib=128)
