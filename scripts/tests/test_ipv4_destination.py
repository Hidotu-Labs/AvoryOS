#!/usr/bin/env python3
"""Run the actual IPv4 destination selection with a mocked link/ARP layer."""
from pathlib import Path
import subprocess, tempfile
source=Path('kernel/src/net/ipv4.c').read_text()
a=source.index('static int ipv4_send_tos(')
b=source.index('  /* Build the whole Ethernet frame',a)
body=source[a:b]+''' memcpy(observed, destination_mac, 6); return 0; }\n'''
harness=r'''
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include <assert.h>
#define NET_MTU_ETHERNET 1500
struct ipv4_header { char bytes[20]; };
struct net_device { int unused; } device;
static struct { uint32_t address,netmask,gateway; } config;
static bool online=true, resolved=true;
static unsigned arp_calls;
static uint32_t arp_target;
static unsigned char observed[6];
static struct net_device *net_device_default(void){return online?&device:0;}
static bool arp_resolve(uint32_t ip,uint8_t *mac){arp_calls++;arp_target=ip;memset(mac,7,6);return resolved;}
'''
checks=r'''
int main(void){
 config.address=0xc0a80102;config.netmask=0xffffff00;config.gateway=0xc0a80101;
 assert(ipv4_send_tos(0xffffffff,17,0,"x",1)==0);
 assert(!arp_calls && observed[0]==255 && observed[5]==255);
 assert(ipv4_send_tos(0xc0a801ff,17,0,"x",1)==0 && !arp_calls);
 assert(ipv4_send_tos(0xe0010203,17,0,"x",1)==0 && !arp_calls);
 assert(!memcmp(observed,"\x01\x00\x5e\x01\x02\x03",6));
 assert(ipv4_send_tos(0xc0a80103,17,0,"x",1)==0 && arp_target==0xc0a80103);
 assert(ipv4_send_tos(0x08080808,17,0,"x",1)==0 && arp_target==config.gateway);
 resolved=false;assert(ipv4_send_tos(0x08080808,17,0,"x",1)==-113);
 config.gateway=0;assert(ipv4_send_tos(0x08080808,17,0,"x",1)==-101);
 online=false;assert(ipv4_send_tos(0xffffffff,17,0,"x",1)==-100);
 assert(ipv4_send_tos(0xffffffff,17,0,"x",1481)==-90);
}
'''
with tempfile.TemporaryDirectory() as tmp:
 p=Path(tmp)/'test.c';p.write_text(harness+body+checks)
 subprocess.run(['cc','-Wall','-Wextra','-Wno-unused-parameter',str(p),'-o',tmp+'/test'],check=True)
 subprocess.run([tmp+'/test'],check=True)
print('PASS: broadcast/multicast MAC selection, unicast ARP and network errors')
