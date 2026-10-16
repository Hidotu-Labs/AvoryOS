#!/usr/bin/env python3
"""Private mount-table, bind permissions, root confinement and devpts checks."""
from boot_probe import run_probe
INIT=r'''
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/reboot.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>
static int failures;
#define CHECK(x) do {if(!(x)){printf("FAIL line=%d errno=%d: %s\n",__LINE__,errno,#x);failures++;}}while(0)
static void touch(const char*p){int f=open(p,O_WRONLY|O_CREAT,0644);CHECK(f>=0);if(f>=0){CHECK(write(f,"sentinel",8)==8);close(f);}}
static int exists(const char*p){return access(p,F_OK)==0;}
static void wait_ok(pid_t p){int s=0;CHECK(waitpid(p,&s,0)==p);CHECK(WIFEXITED(s)&&WEXITSTATUS(s)==0);}
static void child(void){
 CHECK(exists("/tmp/mount-private/host"));
 CHECK(mount("tmpfs","/tmp/mount-private","tmpfs",MS_NOSUID|MS_NODEV,0)==0);
 CHECK(!exists("/tmp/mount-private/host"));touch("/tmp/mount-private/child");
 CHECK(mkdir("/tmp/mount-private/bind",0755)==0);
 CHECK(mount("/tmp/mount-source","/tmp/mount-private/bind",0,MS_BIND,0)==0);
 CHECK(exists("/tmp/mount-private/bind/file"));
 CHECK(mkdir("/tmp/mount-private/bind/sub",0755)==0 || errno==EEXIST);
 CHECK(mount("tmpfs","/tmp/mount-private/bind/sub","tmpfs",0,0)==0);touch("/tmp/mount-private/bind/sub/nested");
 CHECK(mkdir("/tmp/mount-private/copy",0755)==0);
 CHECK(mount("/tmp/mount-private/bind","/tmp/mount-private/copy",0,MS_BIND|MS_REC,0)==0);
 CHECK(exists("/tmp/mount-private/copy/sub/nested"));
 struct {unsigned long set,clear,propagation,userns;} attr={1,0,0,0};
 CHECK(syscall(442,AT_FDCWD,"/tmp/mount-private/copy",0x8000,&attr,sizeof(attr))==0);
 int nested=open("/tmp/mount-private/copy/sub/nested",O_WRONLY);CHECK(nested<0&&errno==EROFS);if(nested>=0)close(nested);
 int directory=open("/tmp/mount-private",O_PATH|O_DIRECTORY);CHECK(directory>=0);
 struct {unsigned long flags,mode,resolve;} how={O_PATH,0,0x10|2};
 int bounded=syscall(437,directory,"/child",&how,sizeof(how));CHECK(bounded>=0);
 if(bounded>=0){char fdpath[128],target[128]={0};snprintf(fdpath,sizeof(fdpath),"/proc/self/fd/%d",bounded);CHECK(readlink(fdpath,target,sizeof(target)-1)>0);printf("BOUNDED_PATH=%s\n",target);CHECK(!strcmp(target,"/tmp/mount-private/child"));close(bounded);}
 how.resolve=8;bounded=syscall(437,directory,"../mount-source/file",&how,sizeof(how));CHECK(bounded<0&&errno==EXDEV);if(bounded>=0)close(bounded);
 how.resolve=0x10|2;bounded=syscall(437,directory,"/../mount-source/file",&how,sizeof(how));CHECK(bounded<0);if(bounded>=0)close(bounded);close(directory);
 CHECK(mount(0,"/tmp/mount-private/bind",0,MS_BIND|MS_REMOUNT|MS_RDONLY,0)==0);
 int f=open("/tmp/mount-private/bind/file",O_WRONLY);CHECK(f<0&&errno==EROFS);if(f>=0)close(f);
 CHECK(chmod("/tmp/mount-private/bind/file",0600)<0);
 f=open("/tmp/mount-private/bind/file",O_RDONLY);CHECK(f>=0);
 if(f>=0){void *m=mmap(0,4096,PROT_READ|PROT_WRITE,MAP_SHARED,f,0);CHECK(m==MAP_FAILED&&errno==EACCES);if(m!=MAP_FAILED)munmap(m,4096);
  m=mmap(0,4096,PROT_READ,MAP_SHARED,f,0);CHECK(m!=MAP_FAILED);if(m!=MAP_FAILED){CHECK(mprotect(m,4096,PROT_READ|PROT_WRITE)<0&&errno==EACCES);munmap(m,4096);}
  m=mmap(0,4096,PROT_READ|PROT_WRITE,MAP_PRIVATE,f,0);CHECK(m!=MAP_FAILED);if(m!=MAP_FAILED){((char*)m)[0]='x';munmap(m,4096);}close(f);
 }
 f=open("/tmp/mount-source/file",O_WRONLY);CHECK(f>=0);if(f>=0)close(f);
 CHECK(mount(0,"/tmp/mount-private/bind",0,MS_BIND|MS_REMOUNT,0)==0);
 touch("/tmp/mount-private/bind/new");CHECK(exists("/tmp/mount-source/new"));
 CHECK(umount2("/tmp/mount-private/bind",MNT_DETACH)==0);
 CHECK(!exists("/tmp/mount-private/bind/file"));
 CHECK(mkdir("/tmp/mount-private/pts",0755)==0);
 CHECK(mount("devpts","/tmp/mount-private/pts","devpts",MS_NOSUID|MS_NOEXEC, "newinstance")==0);
 int master=open("/tmp/mount-private/pts/ptmx",O_RDWR|O_NOCTTY);CHECK(master>=0);
 if(master>=0){unsigned idx=999;CHECK(ioctl(master,0x80045430,&idx)==0);char p[128];snprintf(p,sizeof(p),"/tmp/mount-private/pts/%u",idx);
  int slave=open(p,O_RDWR|O_NOCTTY);CHECK(slave>=0);if(slave>=0)close(slave);
  char host[64];snprintf(host,sizeof(host),"/dev/pts/%u",idx);int leaked=open(host,O_RDWR|O_NOCTTY);CHECK(leaked<0);if(leaked>=0)close(leaked);close(master);
 }
 CHECK(mkdir("/tmp/mount-private/root",0755)==0);
 touch("/tmp/mount-private/root/inside");
 CHECK(symlink("/tmp/mount-source/file","/tmp/mount-private/root/escape")==0);
 CHECK(chdir("/tmp/mount-private/root")==0);
 CHECK(chroot(".")==0);CHECK(chdir("/")==0);
 CHECK(exists("/inside"));CHECK(!exists("/tmp/mount-source/file"));CHECK(!exists("/escape"));
 CHECK(!exists("/dev/null"));CHECK(open("/dev/null",O_RDONLY)<0);
 CHECK(chdir("..")==0);CHECK(exists("/inside"));CHECK(!exists("/tmp/mount-source/file"));
 _exit(failures?1:0);
}
static void pivots(void){
 CHECK(mount("tmpfs","/tmp/mount-private","tmpfs",0,0)==0);CHECK(chdir("/tmp/mount-private")==0);
 CHECK(mkdir("newroot",0755)==0);CHECK(mount("newroot","newroot",0,0xc0ed0000UL|MS_BIND|MS_REC,0)==0);CHECK(mkdir("oldroot",0755)==0);
 CHECK(syscall(SYS_pivot_root,"/tmp/mount-private","oldroot")==0);CHECK(chdir("/")==0);
 CHECK(exists("/oldroot/tmp/mount-source/file"));touch("/newroot/inside");
 CHECK(umount2("/oldroot",MNT_DETACH)==0);CHECK(!exists("/oldroot/tmp/mount-source/file"));
 int old=open("/",O_RDONLY|O_DIRECTORY);CHECK(old>=0);CHECK(chdir("/newroot")==0);
 CHECK(syscall(SYS_pivot_root,".",".")==0);CHECK(fchdir(old)==0);CHECK(umount2(".",MNT_DETACH)==0);CHECK(chdir("/")==0);close(old);
 CHECK(exists("/inside"));CHECK(!exists("/oldroot"));CHECK(!exists("/tmp/mount-source/file"));
 _exit(failures?1:0);
}
int main(void){setvbuf(stdout,NULL,_IOLBF,0);puts("MOUNT_NAMESPACE_BOOT: START");
 mkdir("/tmp/mount-private",0755);mkdir("/tmp/mount-source",0755);touch("/tmp/mount-private/host");touch("/tmp/mount-source/file");
 pid_t p=syscall(SYS_clone,CLONE_NEWNS|SIGCHLD,0,0,0,0);CHECK(p>=0);if(!p)child();if(p>0)wait_ok(p);
 CHECK(exists("/tmp/mount-private/host"));CHECK(!exists("/tmp/mount-private/child"));CHECK(exists("/tmp/mount-source/new"));
 p=syscall(SYS_clone,CLONE_NEWNS|SIGCHLD,0,0,0,0);CHECK(p>=0);if(!p)pivots();if(p>0)wait_ok(p);
 CHECK(exists("/tmp/mount-private/host"));puts(failures?"MOUNT_NAMESPACE_BOOT: FAIL":"MOUNT_NAMESPACE_BOOT: PASS");
 reboot(RB_POWER_OFF);return failures?1:0;}
'''
if __name__=='__main__':run_probe('mount-namespace-boot',INIT,'MOUNT_NAMESPACE_BOOT: PASS')
