#!/usr/bin/env python3
"""Test actual inotify namespace delivery in a private KVM guest.

Uses a fresh private root image, never disk.img or VFIO. Set AVORY_TEST_KERNEL
only to compare a previously built kernel with the same deterministic checks.
"""
from pathlib import Path
import importlib.util
import os
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[2]
OUT = ROOT / 'build/inotify-boot'
OUT.mkdir(parents=True, exist_ok=True)
spec = importlib.util.spec_from_file_location('boot', ROOT / 'scripts/syscall-restart-boot.py')
boot = importlib.util.module_from_spec(spec)
spec.loader.exec_module(boot)
boot.OUT = OUT
boot.INIT = r'''
#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/inotify.h>
#include <sys/reboot.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>
static int failures;
#define CHECK(x) do {if(!(x)){printf("FAIL line=%d errno=%d %s\n",__LINE__,errno,#x);failures++;}}while(0)
static int take(int fd,int wd,unsigned mask,const char *name){
 char buf[8192] __attribute__((aligned(8)));int found=0;
 int available=-1;CHECK(!ioctl(fd,FIONREAD,&available));CHECK(available>0);
 CHECK(ioctl(fd,FIONREAD,(void*)1)==-1&&errno==EFAULT);
 ssize_t n=read(fd,buf,sizeof(buf));CHECK(n==available);
 int remaining=-1;CHECK(!ioctl(fd,FIONREAD,&remaining));CHECK(!remaining);
 if(n<0)return 0;
 for(ssize_t off=0;off<n;){
  struct inotify_event *e=(void*)(buf+off);
  if(off+sizeof(*e)+e->len>(size_t)n)return 0;
  if(e->wd==wd&&(e->mask&mask)==mask&&(!name||(e->len&&!strcmp(e->name,name))))found=1;
  off+=sizeof(*e)+e->len;
 }
 return found;
}
static void *remove_queue(void *path){usleep(200000);return (void*)(long)(unlink(path)?1:0);}
static void run(const char *dir){
 CHECK(!mkdir(dir,0700));
 int fd=inotify_init1(IN_NONBLOCK|IN_CLOEXEC);CHECK(fd>=0);
 int wd=inotify_add_watch(fd,dir,IN_CREATE|IN_DELETE|IN_MOVED_FROM|IN_MOVED_TO|IN_ONLYDIR);CHECK(wd>0);
 char file[256],renamed[256],alias[256];
 snprintf(alias,sizeof(alias),"%s/.",dir);
 CHECK(inotify_add_watch(fd,alias,IN_CREATE|IN_DELETE|IN_MOVED_FROM|IN_MOVED_TO)==wd);
 snprintf(file,sizeof(file),"%s/queue",dir);snprintf(renamed,sizeof(renamed),"%s/moved",dir);
 int f=open(file,O_CREAT|O_WRONLY,0600);CHECK(f>=0);if(f>=0)close(f);
 CHECK(take(fd,wd,IN_CREATE,"queue"));
 char buf[8192];int empty_bytes=-1;CHECK(!ioctl(fd,FIONREAD,&empty_bytes)&&empty_bytes==0);
 CHECK(read(fd,buf,sizeof(buf))==-1&&errno==EAGAIN);
 CHECK(!rename(file,renamed));
 ssize_t n=read(fd,buf,sizeof(buf));unsigned from=0,to=0;
 CHECK(n>0);
 for(ssize_t off=0;off<n;){struct inotify_event *e=(void*)(buf+off);
  if(e->mask&IN_MOVED_FROM)from=e->cookie;
  if(e->mask&IN_MOVED_TO)to=e->cookie;
  off+=sizeof(*e)+e->len;
 }
 CHECK(from&&from==to);
 int self=inotify_add_watch(fd,renamed,IN_DELETE_SELF);CHECK(self>0);
 CHECK(!unlink(renamed));CHECK(take(fd,wd,IN_DELETE,"moved"));
 CHECK(inotify_rm_watch(fd,self)==-1&&errno==EINVAL); /* deleted watch retired */
 f=open(file,O_CREAT|O_WRONLY,0600);CHECK(f>=0);if(f>=0)close(f);(void)take(fd,wd,IN_CREATE,"queue");
 int ep=epoll_create1(EPOLL_CLOEXEC);CHECK(ep>=0);
 struct epoll_event event={.events=EPOLLIN,.data.fd=fd};CHECK(!epoll_ctl(ep,EPOLL_CTL_ADD,fd,&event));
 pthread_t worker;CHECK(!pthread_create(&worker,0,remove_queue,file));
 struct pollfd p={.fd=fd,.events=POLLIN};
 CHECK(poll(&p,1,2000)==1&&(p.revents&POLLIN));
 CHECK(epoll_wait(ep,&event,1,100)==1);
 CHECK(take(fd,wd,IN_DELETE,"queue"));
 void *status=0;CHECK(!pthread_join(worker,&status)&&status==0);
 CHECK(!inotify_rm_watch(fd,wd));CHECK(take(fd,wd,IN_IGNORED,0));
 CHECK(read(fd,buf,sizeof(buf))==-1&&errno==EAGAIN);
 close(ep);close(fd);CHECK(!rmdir(dir));
 printf("INOTIFY_BOOT: finished %s\n",dir);
}
/* Qt/SDDM publishes a socket from a private directory into /tmp. The
 * notification lookup must not leave its new name negatively cached. */
static void socket_move(void){
 const char *dir="/tmp/probe-sddm", *old="/tmp/probe-sddm/s", *dest="/tmp/probe-sddm-socket";
 CHECK(!mkdir(dir,0700));
 int fd=socket(AF_UNIX,SOCK_STREAM,0);CHECK(fd>=0);
 struct sockaddr_un addr={.sun_family=AF_UNIX};strcpy(addr.sun_path,old);
 CHECK(!bind(fd,(void*)&addr,sizeof(addr)));CHECK(!listen(fd,1));
 CHECK(!chmod(old,0700));
 struct stat st;CHECK(lstat(dest,&st)==-1&&errno==ENOENT);
 CHECK(!rename(old,dest));
 CHECK(!lstat(dest,&st)&&S_ISSOCK(st.st_mode));
 CHECK(!chown(dest,963,963));CHECK(!lstat(dest,&st)&&st.st_uid==963&&st.st_gid==963);
 CHECK(!rmdir(dir));
 int client=socket(AF_UNIX,SOCK_STREAM,0);CHECK(client>=0);strcpy(addr.sun_path,dest);
 int connected=connect(client,(void*)&addr,sizeof(addr));CHECK(!connected);
 int peer=connected==0?accept(fd,0,0):-1;if(!connected)CHECK(peer>=0);
 if(peer>=0)close(peer);close(client);close(fd);CHECK(!unlink(dest));
 puts("INOTIFY_BOOT: SDDM socket rename, ownership and connection checked");
}
static void extra(void){
 const char *dir="/probe-queue";CHECK(!mkdir(dir,0700));
 int fd=inotify_init1(IN_NONBLOCK);CHECK(fd>=0);
 int wd=inotify_add_watch(fd,dir,IN_CREATE);CHECK(wd>0);
 int f=open("/probe-queue/alpha",O_CREAT|O_WRONLY,0600);CHECK(f>=0);if(f>=0)close(f);
 char small[16];CHECK(read(fd,small,sizeof(small))==-1&&errno==EINVAL);
 CHECK(take(fd,wd,IN_CREATE,"alpha"));
 char path[128];
 for(int i=0;i<80;i++){snprintf(path,sizeof(path),"%s/f%03d",dir,i);f=open(path,O_CREAT|O_WRONLY,0600);CHECK(f>=0);if(f>=0)close(f);}
 int before=0,after=0;CHECK(!ioctl(fd,FIONREAD,&before));
 char one[32];CHECK(read(fd,one,sizeof(one))==32);
 CHECK(!ioctl(fd,FIONREAD,&after)&&after==before-32);
 CHECK(take(fd,-1,IN_Q_OVERFLOW,0));
 CHECK(inotify_add_watch(fd,dir,IN_CREATE|IN_ONESHOT)==wd);
 f=open("/probe-queue/once",O_CREAT|O_WRONLY,0600);CHECK(f>=0);if(f>=0)close(f);
 CHECK(take(fd,wd,IN_CREATE,"once"));
 CHECK(inotify_rm_watch(fd,wd)==-1&&errno==EINVAL);
 close(fd);
 unlink("/probe-queue/alpha");unlink("/probe-queue/once");
 for(int i=0;i<80;i++){snprintf(path,sizeof(path),"%s/f%03d",dir,i);CHECK(!unlink(path));}
 CHECK(!rmdir(dir));
 puts("INOTIFY_BOOT: small buffers, overflow and one-shot watches checked");
}
static void pidfd_info(void){
 struct info {unsigned long long mask,cgroup;unsigned pid,tgid,ppid,ruid,rgid,euid,egid,suid,sgid,fsuid,fsgid;int exit_code;unsigned core,sig,code,pad;unsigned long long supported;};
 _Static_assert(sizeof(struct info)==88,"ABI");
 int fd=syscall(434,getpid(),0);CHECK(fd>=0);
 struct {struct info i;unsigned long long tail;} out;
 memset(&out,0xa5,sizeof(out));out.i.mask=~0ULL;
 CHECK(!ioctl(fd,0xc060ff0b,&out));
 CHECK(out.i.mask==35&&out.i.supported==35&&out.tail==0);
 CHECK(out.i.pid==getpid()&&out.i.tgid==getpid()&&out.i.ppid==getppid());
 CHECK(out.i.ruid==getuid()&&out.i.rgid==getgid()&&out.i.euid==geteuid()&&out.i.egid==getegid());
 CHECK(out.i.fsuid==getuid()&&out.i.fsgid==getgid());
 CHECK(!out.i.cgroup&&!out.i.exit_code&&!out.i.core&&!out.i.sig&&!out.i.code);
 memset(&out,0xa5,sizeof(out));out.i.mask=0;
 CHECK(!ioctl(fd,0xc040ff0b,&out));CHECK(out.i.mask==3);
 CHECK(out.i.core==0xa5a5a5a5U); /* version 0 must leave later fields untouched */
 CHECK(ioctl(fd,0xc058ff0b,(void*)1)==-1&&errno==EFAULT);
 CHECK(ioctl(fd,0xc058ff0b,0)==-1&&errno==EINVAL);
 CHECK(ioctl(fd,0xc058fe0b,&out)==-1&&errno==ENOTTY);
 close(fd);puts("INOTIFY_BOOT: pidfd information, versions and user buffers checked");
}
int main(void){
 setvbuf(stdout,0,_IOLBF,0);puts("INOTIFY_BOOT: START");
 /* Keep this filesystem-notification test independent of asynchronous
  * default SIGCHLD delivery; udev workers likewise block it for signalfd. */
 sigset_t mask;sigemptyset(&mask);sigaddset(&mask,SIGCHLD);sigprocmask(SIG_BLOCK,&mask,0);
 run("/probe-ext2");run("/tmp/probe-tmpfs");extra();socket_move();pidfd_info();
 printf("INOTIFY_BOOT: END failures=%d\n",failures);fflush(NULL);
 reboot(RB_POWER_OFF);for(;;)sleep(1);
}
'''
run = boot.run
def with_current_kernel(args):
    if str(args[0]) == 'xorriso' and '-as' in args:
        kernel = Path(os.environ.get('AVORY_TEST_KERNEL', str(ROOT / 'kernel/bin-x86_64/kernel')))
        shutil.copy2(kernel, OUT / 'iso-root/boot/kernel')
        (OUT / 'iso-root/boot/kernel').chmod(0o644)
        run(['strip', '--strip-all', OUT / 'iso-root/boot/kernel'])
    run(args)
boot.run = with_current_kernel
disk, iso = boot.prepare()
serial = OUT / 'serial.log'
cmd = ['qemu-system-x86_64','-M','q35','-cpu','host','-enable-kvm','-smp','4','-m','1G',
       '-drive',f'if=pflash,unit=0,format=raw,file={ROOT}/edk2-ovmf/ovmf-code-x86_64.fd,readonly=on',
       '-cdrom',str(iso),'-boot','d','-drive',f'file={disk},format=raw,if=none,id=probe',
       '-device','nvme,serial=inotify-probe,drive=probe','-vga','none',
       '-device','virtio-vga,addr=06.0','-display','none','-serial',f'file:{serial}',
       '-monitor','none','-no-reboot']
with (OUT / 'host.log').open('wb') as output:
    process = subprocess.Popen(cmd, stdout=output, stderr=output)
    try:
        process.wait(timeout=120)
    finally:
        if process.poll() is None:
            process.terminate()
            process.wait(timeout=5)
log = serial.read_text(errors='replace')
assert process.returncode == 0 and 'INOTIFY_BOOT: END failures=0' in log, str(serial)
print(log[log.index('INOTIFY_BOOT: START'):].split('INOTIFY_BOOT: END')[0] + 'INOTIFY_BOOT: END failures=0')
