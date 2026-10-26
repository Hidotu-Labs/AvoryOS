#!/usr/bin/env python3
"""Exercise actual SIOCGIFCONF with configured IPv4 and fault-safe usercopy."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
s = (root/'kernel/src/socket/socket.c').read_text()
body = s[s.index('struct avory_ifconf64 {'):s.index('// Socket Family Registry')]
pre = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
struct net_device;
struct net_device_ops {bool (*link_up)(struct net_device *);};
struct net_device {bool registered;char name[16];uint8_t mac[6];const struct net_device_ops *ops;};
struct ipv4_config {uint32_t address,netmask;};
static struct net_device device={.registered=true,.name="eth0",.mac={2,1,2,3,4,5}};
static struct ipv4_config config={0x0a00020f,0xffffff00};
static bool bad_read,bad_write;
static struct net_device *net_device_default(void){return &device;}
static const struct ipv4_config *ipv4_get_config(void){return &config;}
static bool vmm_is_user_addr_range_valid(uint64_t a,size_t n){return a>=4096&&n;}
static unsigned long copy_from_user(void *to,const void *from,unsigned long n){if(bad_read||(uintptr_t)from<4096)return n;memcpy(to,from,n);return 0;}
static unsigned long copy_to_user(void *to,const void *from,unsigned long n){if(bad_write||(uintptr_t)to<4096)return n;memcpy(to,from,n);return 0;}
'''
main = r'''
int main(void){
 struct avory_ifconf64 c;
 for(int size=0;size<=4096;size+=4096){
  c=(struct avory_ifconf64){.length=size};
  assert(!socket_ioctl_ifconf((uintptr_t)&c)&&c.length==40);
 }
 unsigned char data[80];memset(data,0xa5,sizeof(data));
 for(int size=0;size<=80;size++){
  c=(struct avory_ifconf64){.length=size,.buffer=(uintptr_t)data};
  assert(!socket_ioctl_ifconf((uintptr_t)&c)&&c.length==(size>=40?40:0));
  if(size>=40){struct avory_ifreq64 *r=(void*)data;assert(!strcmp(r->name,"eth0")&&r->family==2&&r->address_data[2]==10&&r->address_data[5]==15);}
  for(unsigned i=40;i<sizeof(data);i++)assert(data[i]==0xa5);
 }
 c=(struct avory_ifconf64){.length=40,.buffer=1};assert(socket_ioctl_ifconf((uintptr_t)&c)==-14);
 c=(struct avory_ifconf64){.length=-1};assert(socket_ioctl_ifconf((uintptr_t)&c)==-22);
 c=(struct avory_ifconf64){0};bad_read=true;assert(socket_ioctl_ifconf((uintptr_t)&c)==-14);bad_read=false;
 bad_write=true;assert(socket_ioctl_ifconf((uintptr_t)&c)==-14);bad_write=false;
 struct avory_ifreq64 req={.name="lo"};
 assert(!socket_ioctl_ifreq(0x8913,(uintptr_t)&req)&&req.family==0x49);
 assert(!socket_ioctl_ifreq(0x891b,(uintptr_t)&req)&&req.family==2&&req.address_data[2]==255&&!req.address_data[3]);
 strcpy(req.name,"eth0");assert(!socket_ioctl_ifreq(0x891b,(uintptr_t)&req));
 assert(req.address_data[2]==255&&req.address_data[3]==255&&req.address_data[4]==255&&!req.address_data[5]);
 assert(!socket_ioctl_ifreq(0x8913,(uintptr_t)&req)&&(req.family&0x1003)==0x1003);
 assert(!socket_ioctl_ifreq(0x8919,(uintptr_t)&req)&&req.family==2&&req.address_data[2]==10&&req.address_data[3]==0&&req.address_data[4]==2&&req.address_data[5]==255);
 assert(!socket_ioctl_ifreq(0x8927,(uintptr_t)&req)&&req.family==1&&!memcmp(req.address_data,device.mac,6));
 strcpy(req.name,"lo");assert(!socket_ioctl_ifreq(0x8927,(uintptr_t)&req)&&req.family==772);for(int i=0;i<14;i++)assert(!req.address_data[i]);
 assert(!socket_ioctl_ifreq(0x8919,(uintptr_t)&req)&&req.family==2);for(int i=0;i<14;i++)assert(!req.address_data[i]);
 strcpy(req.name,"missing");assert(socket_ioctl_ifreq(0x8913,(uintptr_t)&req)==-19);
 req=(struct avory_ifreq64){.name="lo"};assert(!socket_ioctl_ifreq(0x8933,(uintptr_t)&req)&&req.family==1);
 assert(!socket_ioctl_ifreq(0x8910,(uintptr_t)&req)&&!strcmp(req.name,"lo"));
 assert(!socket_ioctl_ifreq(0x8915,(uintptr_t)&req)&&req.address_data[2]==127&&req.address_data[5]==1);
 device.registered=false;c=(struct avory_ifconf64){.length=4096};assert(!socket_ioctl_ifconf((uintptr_t)&c)&&!c.length);
 puts("PASS: configured IPv4 size queries at zero/nonzero length, whole-record truncation, address bytes, no-device and usercopy faults");
}
'''
with tempfile.TemporaryDirectory(prefix='ifconf-') as tmp:
    path = Path(tmp)
    (path/'test.c').write_text(pre+body+main)
    subprocess.run(['cc', '-O2', '-fno-builtin', '-Wall', '-Wextra', '-Werror', str(path/'test.c'), '-o', str(path/'test')], check=True)
    subprocess.run([str(path/'test')], check=True)
