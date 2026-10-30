#!/usr/bin/env python3
"""Exercise native memfd backing, live shared views, COW and close lifetime."""
from boot_probe import run_probe
INIT=r'''
#define _GNU_SOURCE
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/reboot.h>
#include <sys/sysinfo.h>
#include <stdio.h>
#include <stdint.h>
#include <unistd.h>
#include <errno.h>
#define CHECK(x) do { if (!(x)) {printf("MEMFD_PRIVATE_VIEWS: FAIL line=%d errno=%d\n",__LINE__,errno);goto done;} } while(0)
int main(void){setbuf(stdout,NULL);puts("MEMFD_PRIVATE_VIEWS: START");
 for(int i=0;i<64;i++) {
 int fd=syscall(SYS_memfd_create,"graphics",0);CHECK(fd>=0);
 CHECK(ftruncate(fd,768L*1024*1024)==0);
 struct sysinfo before,after;CHECK(sysinfo(&before)==0);
 volatile uint32_t *s=mmap(0,768L*1024*1024,PROT_READ|PROT_WRITE,MAP_SHARED,fd,0);
 volatile uint32_t *p=mmap(0,4096,PROT_READ|PROT_WRITE,MAP_PRIVATE,fd,0);
 CHECK(sysinfo(&after)==0);
 CHECK(before.freeram <= after.freeram+16L*1024*1024);
 CHECK(s!=MAP_FAILED && p!=MAP_FAILED);CHECK(*s==0 && *p==0);
 *s=123;CHECK(*p==123);*p=456;CHECK(*s==123 && *p==456);
 CHECK(ftruncate(fd,1024L*1024*1024)==0);*s=789;
 uint32_t value=0;CHECK(pread(fd,&value,4,0)==4 && value==789);
 CHECK(close(fd)==0);CHECK(*s==789 && *p==456);
 CHECK(munmap((void*)s,768L*1024*1024)==0 && munmap((void*)p,4096)==0);
 }
 puts("MEMFD_PRIVATE_VIEWS: PASS");
 done: reboot(RB_POWER_OFF);for(;;)pause();}
'''
if __name__=='__main__':
 run_probe('memfd-private-views',INIT,'MEMFD_PRIVATE_VIEWS: PASS',cpus=1,timeout=90)
