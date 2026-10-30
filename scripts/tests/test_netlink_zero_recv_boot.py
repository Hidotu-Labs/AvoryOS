#!/usr/bin/env python3
"""Run libtorrent's Netlink dump receive pattern inside AvoryOS."""
from boot_probe import run_probe
INIT = r'''
#define _GNU_SOURCE
#include <sys/socket.h>
#include <sys/reboot.h>
#include <stdint.h>
#define NETLINK_ROUTE 0
#define RTM_GETLINK 18
#define RTM_GETADDR 22
#define RTM_GETROUTE 26
#define NLM_F_DUMP 0x300
#define NLM_F_REQUEST 1
#define NLMSG_DONE 3
#define NLMSG_ERROR 2
struct sockaddr_nl {uint16_t nl_family,pad;uint32_t nl_pid,nl_groups;};
struct nlmsghdr {uint32_t nlmsg_len;uint16_t nlmsg_type,nlmsg_flags;uint32_t nlmsg_seq,nlmsg_pid;};
struct rtgenmsg {unsigned char rtgen_family;};
#define NLMSG_LENGTH(n) (sizeof(struct nlmsghdr)+(n))
#define ALIGN4(n) (((n)+3)&~3)
#define NLMSG_OK(h,n) ((n)>=(int)sizeof(*(h)) && (h)->nlmsg_len>=sizeof(*(h)) && (h)->nlmsg_len<=(unsigned)(n))
#define NLMSG_NEXT(h,n) ((n)-=ALIGN4((h)->nlmsg_len),(struct nlmsghdr*)((char*)(h)+ALIGN4((h)->nlmsg_len)))
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
int main(void) {
 setbuf(stdout,NULL); puts("NETLINK_ZERO_RECV_BOOT: START");
 int failures=0;
 int fd=socket(AF_NETLINK,SOCK_RAW,NETLINK_ROUTE);
 struct sockaddr_nl local={.nl_family=AF_NETLINK};
 if(fd<0 || bind(fd,(struct sockaddr*)&local,sizeof(local))) failures++;
 socklen_t length=sizeof(local);
 if(getsockname(fd,(struct sockaddr*)&local,&length)) failures++;
 int types[]={RTM_GETLINK,RTM_GETADDR,RTM_GETROUTE};
 for(unsigned i=0;i<3;i++) {
  struct {struct nlmsghdr h;struct rtgenmsg g;} request={0};
  request.h.nlmsg_len=NLMSG_LENGTH(sizeof(request.g));
  request.h.nlmsg_type=types[i];request.h.nlmsg_flags=NLM_F_DUMP|NLM_F_REQUEST;
  request.h.nlmsg_seq=i+1;request.g.rtgen_family=AF_UNSPEC;
  if(send(fd,&request,request.h.nlmsg_len,0)<0){failures++;continue;}
  int done=0;
  for(int j=0;j<16 && !done;j++) {
   ssize_t size=recv(fd,NULL,0,MSG_PEEK|MSG_TRUNC);
   char buffer[4096];
   if(size<=0 || size>sizeof(buffer)){printf("probe size=%ld errno=%d\n",(long)size,errno);failures++;break;}
   if(recv(fd,NULL,0,MSG_PEEK|MSG_TRUNC)!=size) failures++;
   ssize_t count=recv(fd,buffer,size,MSG_TRUNC);
   if(count!=size){failures++;break;}
   int left=count;
   for(struct nlmsghdr *h=(void*)buffer;NLMSG_OK(h,left);h=NLMSG_NEXT(h,left)){
    if(h->nlmsg_seq!=i+1 || h->nlmsg_pid!=local.nl_pid) failures++;
    if(h->nlmsg_type==NLMSG_DONE)done=1;
    if(h->nlmsg_type==NLMSG_ERROR)failures++;
   }
  }
  if(!done)failures++;
 }
 close(fd);
 printf("NETLINK_ZERO_RECV_BOOT: %s failures=%d\n",failures?"FAIL":"PASS",failures);
 reboot(RB_POWER_OFF);for(;;)pause();
}
'''
if __name__ == '__main__':
    run_probe('netlink-zero-recv-boot',INIT,'NETLINK_ZERO_RECV_BOOT: PASS',timeout=90)
