#!/usr/bin/env python3
"""Boot-test capability drops, bounding sets, securebits and network isolation."""
from boot_probe import run_probe
INIT = r'''
#define _GNU_SOURCE
#include <errno.h>
#include <grp.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/prctl.h>
#include <sys/reboot.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <net/if.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <arpa/inet.h>
#include <unistd.h>
struct hdr { unsigned version; int pid; };
struct data { unsigned effective, permitted, inheritable; };
static int failures;
#define CHECK(x) do { if(!(x)) {printf("FAIL line=%d errno=%d: %s\n",__LINE__,errno,#x); failures++;}} while(0)
static int caps(struct data *d) {struct hdr h={0x20080522,0};return syscall(SYS_capset,&h,d);}
static void waitok(pid_t p) {int st;CHECK(waitpid(p,&st,0)==p && WIFEXITED(st) && WEXITSTATUS(st)==0);}
static int abstract_socket(void) {
 int fd=socket(AF_UNIX,SOCK_DGRAM,0);CHECK(fd>=0);
 struct sockaddr_un addr={.sun_family=AF_UNIX};memcpy(addr.sun_path+1,"private-net-check",17);
 CHECK(bind(fd,(void*)&addr,2+18)==0);return fd;
}
static void netrequest(int fd,int type,void *data,size_t size) {
 unsigned char buffer[128]={0}; struct nlmsghdr *h=(void*)buffer;
 h->nlmsg_len=sizeof(*h)+size;h->nlmsg_type=type;h->nlmsg_flags=NLM_F_REQUEST|NLM_F_ACK|NLM_F_CREATE|NLM_F_EXCL;
 h->nlmsg_seq=77;h->nlmsg_pid=getpid();memcpy(buffer+sizeof(*h),data,size);
 struct sockaddr_nl dest={.nl_family=AF_NETLINK};
 CHECK(sendto(fd,buffer,h->nlmsg_len,0,(void*)&dest,sizeof(dest))==(ssize_t)h->nlmsg_len);
 int n=recv(fd,buffer,sizeof(buffer),0);CHECK(n>=36);
 h=(void*)buffer;CHECK(h->nlmsg_type==NLMSG_ERROR && h->nlmsg_seq==77);
 int error;memcpy(&error,buffer+sizeof(*h),4);CHECK(error==0);
}
int main(int argc,char **argv) {
 setvbuf(stdout,NULL,_IOLBF,0);puts("CAP_NET_BOOT: START");
 if(argc>1) {
  struct hdr h={0x20080522,0};struct data d[2];CHECK(syscall(SYS_capget,&h,d)==0);
  CHECK(!(d[0].effective|d[1].effective|d[0].permitted|d[1].permitted));
  CHECK(unshare(CLONE_NEWUTS)==-1 && errno==EPERM);
  _exit(failures?1:0);
 }
 struct hdr h={0x20080522,0};struct data d[2];CHECK(syscall(SYS_capget,&h,d)==0);
 CHECK(d[0].effective==0xffffffffu && d[1].effective==0x1ff);
 pid_t p=fork();CHECK(p>=0);
 if(!p) {
  CHECK(prctl(PR_CAPBSET_READ,32)==1); /* Deliberately omitted unused varargs. */
  for(int c=0;c<=40;c++) CHECK(prctl(PR_CAPBSET_DROP,c)==0);
  CHECK(prctl(PR_CAPBSET_READ,21)==0);
  struct data zero[2]={0};CHECK(caps(zero)==0);
  CHECK(caps(d)==-1 && errno==EPERM);
  CHECK(setuid(1234)==-1 && errno==EPERM);
  CHECK(setgid(1234)==-1 && errno==EPERM);
  CHECK(setgroups(0,NULL)==-1 && errno==EPERM);
  CHECK(unshare(CLONE_NEWUTS)==-1 && errno==EPERM);
  CHECK(prctl(PR_CAPBSET_DROP,1)==-1 && errno==EPERM);
  CHECK(reboot(RB_AUTOBOOT)==-1 && errno==EPERM);
  execl("/bin/probe-init","/bin/probe-init","after-drop",NULL);CHECK(0);_exit(1);
 }
 waitok(p);
 p=fork();CHECK(p>=0);
 if(!p) {
  struct data keep[2]={0};keep[0].effective=keep[0].permitted=keep[0].inheritable=1u<<21;
  CHECK(caps(keep)==0);CHECK(prctl(PR_CAP_AMBIENT,PR_CAP_AMBIENT_RAISE,21,0,0)==0);
  CHECK(prctl(PR_CAP_AMBIENT,PR_CAP_AMBIENT_IS_SET,21,0,0)==1);
  keep[0].inheritable=0;CHECK(caps(keep)==0);
  CHECK(prctl(PR_CAP_AMBIENT,PR_CAP_AMBIENT_IS_SET,21,0,0)==0);
  CHECK(prctl(PR_CAP_AMBIENT,PR_CAP_AMBIENT_RAISE,21,0,0)==-1 && errno==EPERM);
  _exit(failures?1:0);
 }
 waitok(p);
 int filesystem=socket(AF_UNIX,SOCK_DGRAM|SOCK_NONBLOCK,0);CHECK(filesystem>=0);
 struct sockaddr_un fileaddr={.sun_family=AF_UNIX};strcpy(fileaddr.sun_path,"/root/cap-net-socket");
 CHECK(bind(filesystem,(void*)&fileaddr,sizeof(fileaddr))==0);
 CHECK(sendto(filesystem,"a",1,0,(void*)&fileaddr,sizeof(fileaddr))==1);
 char byte=0;CHECK(recv(filesystem,&byte,1,0)==1 && byte=='a');
 int original=abstract_socket();
 int original_inet=socket(AF_INET,SOCK_DGRAM,0);CHECK(original_inet>=0);
 p=fork();CHECK(p>=0);
 if(!p) {
  CHECK(unshare(CLONE_NEWNET)==0);
  int fs_sender=socket(AF_UNIX,SOCK_DGRAM,0);CHECK(fs_sender>=0);
  CHECK(sendto(fs_sender,"b",1,0,(void*)&fileaddr,sizeof(fileaddr))==1);close(fs_sender);
  int local=abstract_socket();CHECK(local>=0); /* Same abstract name allowed in new namespace. */
  int ip=socket(AF_INET,SOCK_DGRAM,0);CHECK(ip>=0);
  struct ifreq ifr={0};strcpy(ifr.ifr_name,"lo");CHECK(ioctl(ip,SIOCGIFINDEX,&ifr)==0 && ifr.ifr_ifindex==1);
  strcpy(ifr.ifr_name,"lo");CHECK(ioctl(ip,SIOCGIFFLAGS,&ifr)==0 && !(ifr.ifr_flags&IFF_UP));
  int nl=socket(AF_NETLINK,SOCK_RAW,NETLINK_ROUTE);CHECK(nl>=0);
  struct sockaddr_nl addr={.nl_family=AF_NETLINK,.nl_pid=getpid()};CHECK(bind(nl,(void*)&addr,sizeof(addr))==0);
  struct {struct ifaddrmsg info;struct rtattr attr;uint32_t address;} a={0};
  a.info.ifa_family=AF_INET;a.info.ifa_prefixlen=8;a.info.ifa_index=1;
  a.attr.rta_len=8;a.attr.rta_type=IFA_LOCAL;a.address=htonl(INADDR_LOOPBACK);
  netrequest(nl,RTM_NEWADDR,&a,sizeof(a));
  struct ifinfomsg info={.ifi_index=1,.ifi_flags=IFF_UP,.ifi_change=IFF_UP};netrequest(nl,RTM_NEWLINK,&info,sizeof(info));
  strcpy(ifr.ifr_name,"lo");CHECK(ioctl(ip,SIOCGIFFLAGS,&ifr)==0 && (ifr.ifr_flags&IFF_UP));
  struct sockaddr_in target={.sin_family=AF_INET,.sin_port=htons(1234),.sin_addr.s_addr=htonl(0x0a000202)};
  CHECK(sendto(ip,"x",1,0,(void*)&target,sizeof(target))==-1 && errno==ENETUNREACH);
  close(nl);close(ip);close(local);_exit(failures?1:0);
 }
 waitok(p);CHECK(recv(filesystem,&byte,1,0)==1 && byte=='b');close(filesystem);close(original_inet);close(original);
 printf("CAP_NET_BOOT: END failures=%d\n",failures);fflush(stdout);
 reboot(RB_AUTOBOOT);return failures?1:0;
}
'''
if __name__ == "__main__":
    run_probe("cap-net-boot",INIT,"CAP_NET_BOOT: END failures=0")
