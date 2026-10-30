#!/usr/bin/env python3
"""x86-64 MAP_32BIT must keep Mono-style executable allocations below 2 GiB."""
from boot_probe import run_probe
INIT = r'''
#define _GNU_SOURCE
#include <sys/mman.h>
#include <sys/reboot.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>
#ifndef MAP_32BIT
#define MAP_32BIT 0x40
#endif
#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE 0x100000
#endif
static void finish(int ok,const char *why){printf("MAP32_BOOT: %s %s\n",ok?"PASS":"FAIL",why);reboot(RB_POWER_OFF);for(;;)pause();}
static int target(void){return 42;}
static int low(void *p,size_t n){return p!=MAP_FAILED && (uintptr_t)p>=65536 && (uintptr_t)p+n<=0x80000000ULL;}
int main(void){setbuf(stdout,NULL);puts("MAP32_BOOT: START");
 int flags=MAP_PRIVATE|MAP_ANONYMOUS|MAP_32BIT;
 unsigned char *code=mmap(NULL,4096,PROT_READ|PROT_WRITE|PROT_EXEC,flags,-1,0);
 if(!low(code,4096))finish(0,"initial executable mapping above 2 GiB");
 intptr_t delta=(intptr_t)(void *)target-(intptr_t)(code+5);
 if(delta!=(int32_t)delta)finish(0,"Mono branch displacement overflow");
 code[0]=0xe9;int32_t displacement=delta;memcpy(code+1,&displacement,4);
 if(((int(*)(void))code)()!=42)finish(0,"execute relative branch");
 void *other=mmap(code,4096,PROT_READ|PROT_WRITE,flags,-1,0);
 if(!low(other,4096)||other==code)finish(0,"overlapping hint");
 void *high=mmap((void *)0x7f1000000000ULL,4096,PROT_READ,flags,-1,0);
 if(!low(high,4096))finish(0,"out-of-range hint");
 int fd=open("/root/data",O_RDONLY);
 if(fd<0)finish(0,"file fixture");
 char *file=mmap(NULL,4096,PROT_READ,MAP_PRIVATE|MAP_32BIT,fd,0);
 if(!low(file,4096)||file[0]!=19)finish(0,"file-backed low mapping");
 void *fixed=mmap((void *)0x500000000ULL,4096,PROT_READ|PROT_WRITE,flags|MAP_FIXED,-1,0);
 if(fixed!=(void *)0x500000000ULL)finish(0,"MAP_FIXED must ignore MAP_32BIT");
 munmap(code,4096);munmap(other,4096);munmap(high,4096);munmap(file,4096);munmap(fixed,4096);close(fd);
 void *reserve=mmap((void *)0x40000000ULL,0x40000000ULL,PROT_NONE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED_NOREPLACE,-1,0);
 if(reserve!=(void *)0x40000000ULL)finish(0,"reserve low address space");
 errno=0;if(mmap(NULL,4096,PROT_READ,flags,-1,0)!=MAP_FAILED||errno!=ENOMEM)finish(0,"low exhaustion must not fall back high");
 munmap(reserve,0x40000000ULL);
 finish(1,"low JIT code, hints, files, fixed mapping and exhaustion");
}
'''
def setup(root):
    (root/'root/data').write_bytes(bytes([19])+bytes(4095))

if __name__=='__main__':
    run_probe('map32-boot',INIT,'MAP32_BOOT: PASS',setup=setup,accel='tcg',cpus=1,timeout=90)
