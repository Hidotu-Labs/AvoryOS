#!/usr/bin/env python3
"""Private VM checks for JVM interface enumeration and TCP linger readback."""
import os
from boot_probe import run_probe

INIT = r'''
#define _GNU_SOURCE
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <sys/reboot.h>
#include <sys/mount.h>
#include <netinet/in.h>
#include <net/if.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
static int failures;
#define CHECK(x) do {if(!(x)){printf("SOCKET_COMPAT_BOOT: FAIL line=%d errno=%d\n",__LINE__,errno);failures++;}}while(0)
int main(void){
 setvbuf(stdout,NULL,_IOLBF,0);puts("SOCKET_COMPAT_BOOT: START");
 mount("devtmpfs","/dev","devtmpfs",0,NULL);
 for(int family=AF_INET;family<=AF_INET6;family+=AF_INET6-AF_INET){
  int fd=socket(family,SOCK_STREAM,0);CHECK(fd>=0);if(fd<0)continue;
  struct linger value={-1,-1};socklen_t length=sizeof(value);
  CHECK(!getsockopt(fd,SOL_SOCKET,SO_LINGER,&value,&length));
  CHECK(length==sizeof(value)&&value.l_onoff==0&&value.l_linger==0);
  int values[]={0,19,5000};
  for(unsigned i=0;i<3;i++){
   value=(struct linger){1,values[i]};CHECK(!setsockopt(fd,SOL_SOCKET,SO_LINGER,&value,sizeof(value)));
   memset(&value,0,sizeof(value));length=sizeof(value);
   CHECK(!getsockopt(fd,SOL_SOCKET,SO_LINGER,&value,&length));
   CHECK(value.l_onoff==1&&value.l_linger==(values[i]>3600?3600:values[i]));
  }
  value=(struct linger){0,9};CHECK(!setsockopt(fd,SOL_SOCKET,SO_LINGER,&value,sizeof(value)));
  value=(struct linger){-1,-1};length=sizeof(value);
  CHECK(!getsockopt(fd,SOL_SOCKET,SO_LINGER,&value,&length)&&!value.l_onoff&&!value.l_linger);
  if(family==AF_INET){
   for(int capacity=0;capacity<=4096;capacity+=4096){
    struct ifconf query={.ifc_len=capacity,.ifc_buf=NULL};
    CHECK(!ioctl(fd,SIOCGIFCONF,&query));CHECK(query.ifc_len>=0&&query.ifc_len%sizeof(struct ifreq)==0);
    if(query.ifc_len){
     char buffer[4096];memset(buffer,0xa5,sizeof(buffer));
     struct ifconf result={.ifc_len=sizeof(buffer),.ifc_buf=buffer};
     CHECK(!ioctl(fd,SIOCGIFCONF,&result));CHECK(result.ifc_len==query.ifc_len);
     CHECK(((struct ifreq*)buffer)->ifr_addr.sa_family==AF_INET);
    }
   }
   struct ifreq req={0};strcpy(req.ifr_name,"lo");
   CHECK(!ioctl(fd,SIOCGIFBRDADDR,&req)&&req.ifr_broadaddr.sa_family==AF_INET);
   CHECK(!((struct sockaddr_in*)&req.ifr_broadaddr)->sin_addr.s_addr);
   CHECK(!ioctl(fd,SIOCGIFHWADDR,&req)&&req.ifr_hwaddr.sa_family==772);
   for(unsigned i=0;i<6;i++)CHECK(!req.ifr_hwaddr.sa_data[i]);
   struct ifconf invalid={.ifc_len=-1,.ifc_buf=NULL};errno=0;
   CHECK(ioctl(fd,SIOCGIFCONF,&invalid)==-1&&errno==EINVAL);
   errno=0;CHECK(ioctl(fd,SIOCGIFCONF,(void*)1)==-1&&errno==EFAULT);
  }
  close(fd);
 }
 printf("SOCKET_COMPAT_BOOT: %s failures=%d\n",failures?"FAIL":"PASS",failures);
 reboot(RB_POWER_OFF);for(;;)pause();
}
'''
if __name__ == '__main__':
    run_probe('socket-compat-boot', INIT, 'SOCKET_COMPAT_BOOT: PASS', timeout=180,
              accel=os.environ.get('AVORY_TEST_ACCEL', 'kvm'))
