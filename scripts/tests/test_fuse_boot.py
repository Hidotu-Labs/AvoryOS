#!/usr/bin/env python3
"""Native FUSE protocol client/server, read-only VFS and disconnect on private disk."""
import os
import subprocess
from boot_probe import ROOT, run_probe
INIT=r'''
#define _GNU_SOURCE
#include "FUSE_ABI_HEADER"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <dirent.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/reboot.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/wait.h>
#include <sys/uio.h>
#include <poll.h>
#include <unistd.h>
#define FILE_SIZE 196621u
static unsigned char incoming[131072],reply[131072],data[FILE_SIZE];
static unsigned char expected(unsigned i){return (unsigned char)(i*73+i/251);}
static void check(int ok,const char *s){if(!ok){printf("FUSE_BOOT: FAIL %s errno=%d\n",s,errno);fflush(stdout);_exit(1);}}
static void send_reply(int fd,uint64_t id,int error,const void *p,unsigned size){
 struct afuse_out_header h={.len=sizeof(h)+size,.unique=id,.error=error};
 memcpy(reply,&h,sizeof(h));if(size)memcpy(reply+sizeof(h),p,size);
 struct iovec vec[2]={{reply,sizeof(h)},{reply+sizeof(h),size}};
 check(writev(fd,vec,2)==h.len,"daemon vectored reply");
}
static void server(int fd){
 for(;;){
  struct iovec receive[2]={{incoming,40},{incoming+40,sizeof(incoming)-40}};
  ssize_t got=readv(fd,receive,2);if(got<0&&errno==EINTR)continue;
  if(got<0&&errno==ENODEV)break;
  check(got>=40,"daemon receive");struct afuse_in_header *h=(void*)incoming;
  check(h->len==(unsigned)got,"request size");void *body=incoming+40;
  switch(h->opcode){
   case AFUSE_INIT:{
    struct afuse_init_in *i=body;check(i->major==7&&i->minor==26,"INIT version");
    struct afuse_init_out o={.major=7,.minor=26,.max_write=65536};
    struct afuse_out_header bad={.len=16,.unique=h->unique,.error=1};
    check(write(fd,&bad,sizeof(bad))<0&&errno==EINVAL,"positive error rejected");
    bad.error=0;bad.unique+=100;check(write(fd,&bad,sizeof(bad))<0&&errno==ENOENT,"unknown reply rejected");
    send_reply(fd,h->unique,0,&o,sizeof(o));break;
   }
   case AFUSE_GETATTR:{struct afuse_attr_out o={.attr={.ino=1,.mode=0040755,.nlink=2,.blksize=4096}};send_reply(fd,h->unique,0,&o,sizeof(o));break;}
   case AFUSE_LOOKUP:{
    const char *name=body;uint64_t nid=!strcmp(name,"payload")?2:!strcmp(name,"link")?3:!strcmp(name,"disconnect")?4:0;
    if(!nid){send_reply(fd,h->unique,-ENOENT,NULL,0);break;}
    struct afuse_entry_out o={.nodeid=nid,.generation=1,.attr={.ino=nid,.size=nid==3?7:FILE_SIZE,.mode=nid==3?0120777:0100555,.nlink=1,.blksize=4096}};
    send_reply(fd,h->unique,0,&o,sizeof(o));break;
   }
   case AFUSE_OPEN:case AFUSE_OPENDIR:{struct afuse_open_out o={.fh=h->nodeid+100};send_reply(fd,h->unique,0,&o,sizeof(o));break;}
   case AFUSE_READ:{
    struct afuse_read_in *i=body;check(i->fh==h->nodeid+100,"READ handle");
    if(h->nodeid==4){close(fd);_exit(0);}
    unsigned n=i->offset>=FILE_SIZE?0:FILE_SIZE-i->offset;if(n>i->size)n=i->size;
    for(unsigned j=0;j<n;j++)data[j]=expected(i->offset+j);
    send_reply(fd,h->unique,0,data,n);break;
   }
   case AFUSE_READLINK:send_reply(fd,h->unique,0,"payload",7);break;
   case AFUSE_READDIR:{
    struct afuse_read_in *i=body;unsigned used=0;const char *names[]={".","..","payload","link","disconnect"};
    for(unsigned k=i->offset;k<5;k++){
     struct afuse_dirent *d=(void*)(data+used);d->ino=k<2?1:k==2?2:k==3?3:4;d->off=k+1;d->type=k<2?4:k==3?10:8;d->namelen=strlen(names[k]);
     memcpy(d->name,names[k],d->namelen);unsigned record=(24+d->namelen+7)&~7u;memset(data+used+24+d->namelen,0,record-24-d->namelen);used+=record;
    }send_reply(fd,h->unique,0,data,used);break;
   }
   case AFUSE_STATFS:{struct afuse_kstatfs o={.blocks=100,.bsize=4096,.frsize=4096,.namelen=127};send_reply(fd,h->unique,0,&o,sizeof(o));break;}
   case AFUSE_RELEASE:case AFUSE_RELEASEDIR:send_reply(fd,h->unique,0,NULL,0);break;
   case AFUSE_FORGET:break;
   default:send_reply(fd,h->unique,-ENOSYS,NULL,0);break;
  }
 }close(fd);_exit(0);
}
int main(void){
 setvbuf(stdout,NULL,_IOLBF,0);puts("FUSE_BOOT: START");
 int idle=open("/dev/fuse",O_RDWR|O_NONBLOCK);check(idle>=0,"device exists");
 check(read(idle,data,sizeof(data))<0&&errno==EAGAIN,"nonblocking empty read");
 check(!fcntl(idle,F_SETFL,0)&&!fcntl(idle,F_SETFL,O_NONBLOCK),"toggle nonblocking");
 check(read(idle,data,sizeof(data))<0&&errno==EAGAIN,"fcntl nonblocking read");close(idle);
 mkdir("/mnt",0755);int fd=open("/dev/fuse",O_RDWR);check(fd>=0,"open connection");
 char opts[128];snprintf(opts,sizeof(opts),"fd=%d,rootmode=40000,user_id=0,group_id=0,max_read=65536",fd);
 check(mount("test","/mnt","fuse.test",0,opts)<0&&errno==EROFS,"writable mount rejected");
 check(mount("test","/mnt","fuse.test",MS_RDONLY|MS_NODEV|MS_NOSUID,opts)==0,"mount");
 check(read(fd,data,1)<0&&errno==EINVAL,"short receive preserves request");
 pid_t daemon=fork();check(daemon>=0,"fork daemon");if(!daemon)server(fd);close(fd);
 int file=open("/mnt/payload",O_RDONLY);check(file>=0,"lookup payload");
 check(umount("/mnt")<0&&errno==EBUSY,"busy unmount preserves open file");
 unsigned pos=0;while(pos<FILE_SIZE){ssize_t n=read(file,data,FILE_SIZE-pos);check(n>0,"read file");for(unsigned j=0;j<(unsigned)n;j++)check(data[j]==expected(pos+j),"read bytes");pos+=n;}
 check(read(file,data,1)==0,"EOF");
 unsigned char *map=mmap(NULL,FILE_SIZE,PROT_READ,MAP_PRIVATE,file,0);check(map!=MAP_FAILED,"mmap");
 for(unsigned j=0;j<FILE_SIZE;j++)check(map[j]==expected(j),"mapped bytes");munmap(map,FILE_SIZE);close(file);
 char link[32]={0};check(readlink("/mnt/link",link,sizeof(link))==7&&!strcmp(link,"payload"),"readlink");
 file=open("/mnt/link",O_RDONLY);check(file>=0&&read(file,data,17)==17,"symlink traversal");close(file);
 DIR *dir=opendir("/mnt");check(dir!=NULL,"opendir");int entries=0;struct dirent *de;while((de=readdir(dir)))entries++;closedir(dir);check(entries==5,"directory entries");
 struct statfs st;check(!statfs("/mnt",&st)&&st.f_type==0x65735546&&st.f_bsize==4096,"statfs");
 check(open("/mnt/payload",O_WRONLY)<0&&errno==EROFS,"read-only enforcement");
 check(open("/mnt/missing",O_RDONLY)<0&&errno==ENOENT,"negative lookup");
 puts("FUSE_BOOT: mount/read/mmap/link/readdir/statfs passed");
 file=open("/mnt/disconnect",O_RDONLY);check(file>=0,"disconnect fixture");
 check(read(file,data,17)<0&&errno==ENOTCONN,"daemon close wakes pending read");close(file);
 int status;check(waitpid(daemon,&status,0)==daemon&&WIFEXITED(status)&&!WEXITSTATUS(status),"daemon exit");
 check(umount("/mnt")==0,"normal unmount disconnected connection");
 check(open("/mnt/payload",O_RDONLY)<0&&errno==ENOENT,"mount no longer visible");
 puts("FUSE_BOOT: PASS");fflush(stdout);reboot(RB_POWER_OFF);for(;;)pause();
}
'''.replace('FUSE_ABI_HEADER',str(ROOT/'kernel/src/fs/fuse_protocol.h'))
def prepare(disk,out):
 subprocess.run(['mke2fs','-q','-F','-t','ext2','-O','none,filetype,sparse_super','-d',str(out/'root'),str(disk)],check=True)
if __name__=='__main__':
 name=os.environ.get('AVORY_FUSE_TEST_NAME','fuse-boot')
 prefix=name.upper().replace('-','_')
 run_probe(name,INIT.replace('FUSE_BOOT',prefix),prefix+': PASS',prepare_disk=prepare,cpus=2,timeout=120,accel=os.environ.get('AVORY_TEST_ACCEL','tcg'))
