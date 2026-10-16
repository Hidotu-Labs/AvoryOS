#!/usr/bin/env python3
"""Validate the real RTM_NEWLINK messages used by NetworkManager discovery."""
from pathlib import Path
import subprocess,tempfile,sys
ROOT=Path(__file__).resolve().parents[2]
s=Path(sys.argv[1]).read_text() if len(sys.argv)>1 else (ROOT/'kernel/src/socket/af_netlink.c').read_text()
def function(sig):
 a=s.index(sig);b=s.index('{',a)+1;d=1
 while d:d+=(s[b]=='{')-(s[b]=='}');b+=1
 return s[a:b]+'\n'
prefix=s[s.index('struct nlmsghdr_min {'):s.index('static size_t route_attr(')]
c=r'''
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "kernel/src/net/core.h"
typedef struct {int unused;} netlink_sock_t;
static uint8_t packet[512];static size_t packet_len;
static int route_queue(netlink_sock_t *s,const void *p,size_t n){(void)s;assert(n<=sizeof(packet));memcpy(packet,p,n);packet_len=n;return 0;}
'''+prefix+function('static size_t route_attr(')+function('static size_t route_build_link(')+function('static int route_link(')+function('static int route_loopback_link(')+r'''
static const struct ifinfomsg_min *check(uint16_t type,int index){
 struct nlmsghdr_min *h=(void*)packet;assert(h->nlmsg_type==16 && h->nlmsg_seq==42 && h->nlmsg_pid==123 && h->nlmsg_len==packet_len);
 struct ifinfomsg_min *i=(void*)(h+1);assert(i->type==type && i->index==index);return i;
}
static void attr(uint16_t type,const void *value,size_t len){
 for(size_t pos=sizeof(struct nlmsghdr_min)+sizeof(struct ifinfomsg_min);pos<packet_len;){
  const struct rtattr_min *a=(const void*)(packet+pos);assert(a->len>=4 && pos+a->len<=packet_len);
  if(a->type==type){assert(a->len==4+len && !memcmp(a+1,value,len));return;}
  pos+=NL_ALIGN(a->len);
 }
 assert(!"missing attribute");
}
static bool link_up(struct net_device *dev){return dev->registered;}
int main(void){
 const struct net_device_ops ops={.link_up=link_up};
 netlink_sock_t socket;struct net_device dev={.name="eth0",.mac={0x52,0x54,0,0x12,0x34,0x56},.mtu=1500,.ops=&ops,.registered=true};
 assert(!route_link(&socket,42,123,&dev));const struct ifinfomsg_min *i=check(1,2);assert(i->flags & IFF_BROADCAST);
 uint32_t mtu=dev.mtu;attr(IFLA_IFNAME,"eth0",5);attr(IFLA_MTU,&mtu,sizeof(mtu));attr(IFLA_ADDRESS,dev.mac,6);
 uint8_t carrier=1,operstate=6;attr(IFLA_CARRIER,&carrier,1);attr(IFLA_OPERSTATE,&operstate,1);assert(i->flags & IFF_LOWER_UP);
 /* Exercise Linux's ABI offsets and >32-bit counters independently of the
  * kernel's stats structure, then re-query to reject a static snapshot. */
 dev.stats.rx_bytes=0x100000042ULL;dev.stats.tx_bytes=0x200000084ULL;
 dev.stats.rx_packets=11;dev.stats.tx_packets=12;
 dev.stats.rx_errors=13;dev.stats.tx_errors=14;
 dev.stats.rx_dropped=15;dev.stats.tx_dropped=16;
 uint64_t expected[25]={11,12,0x100000042ULL,0x200000084ULL,13,14,15,16};
 assert(!route_link(&socket,42,123,&dev));attr(23,expected,sizeof(expected));
 dev.stats.rx_bytes+=1500;expected[2]+=1500;
 assert(!route_link(&socket,42,123,&dev));attr(23,expected,sizeof(expected));
 dev.registered=false;assert(!route_link(&socket,42,123,&dev));i=check(1,2);assert(!(i->flags & (IFF_LOWER_UP|IFF_RUNNING)));carrier=0;operstate=2;attr(IFLA_CARRIER,&carrier,1);attr(IFLA_OPERSTATE,&operstate,1);
 assert(!route_loopback_link(&socket,42,123));i=check(772,1);assert(i->flags & IFF_LOOPBACK);attr(IFLA_IFNAME,"lo",3);
 puts("PASS: RTM_NEWLINK Ethernet/loopback hardware types, identifiers and attributes");
}
'''
with tempfile.TemporaryDirectory(prefix='avory-link-types-') as td:
 p=Path(td);(p/'test.c').write_text(c)
 subprocess.run(['cc','-std=gnu11','-O2','-Wall','-Wextra','-Werror','-I',str(ROOT),p/'test.c','-o',p/'test'],check=True)
 subprocess.run([p/'test'],check=True)
