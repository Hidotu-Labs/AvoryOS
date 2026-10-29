#!/usr/bin/env python3
"""Measure browser-style mprotect merges with many live mappings in KVM."""
import os
from boot_probe import run_probe

INIT = r'''
#define _GNU_SOURCE
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/mman.h>
#include <sys/reboot.h>
#include <time.h>
#include <unistd.h>
static volatile int stop, ready;
static uint64_t worst;
static uint64_t ns(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return (uint64_t)t.tv_sec*1000000000+t.tv_nsec;}
static void *monitor(void *p){(void)p;__atomic_store_n(&ready,1,__ATOMIC_RELEASE);while(!__atomic_load_n(&stop,__ATOMIC_RELAXED)){uint64_t t=ns();usleep(1000);uint64_t d=ns()-t;if(d>worst)worst=d;}return NULL;}
int main(void){
 setbuf(stdout,NULL);puts("VMA_MERGE_LATENCY_BOOT: START");
 const size_t count=2000,bytes=count*8192;
 unsigned char *p=mmap(NULL,bytes,PROT_NONE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
 if(p==MAP_FAILED)goto fail;
 /* Every other page stays inaccessible, retaining separate VMA intervals. */
 for(size_t i=0;i<count;i++){
  if(mprotect(p+i*8192,4096,PROT_READ|PROT_WRITE))goto fail;
  p[i*8192]=(unsigned char)i;
 }
 pthread_t thread;if(pthread_create(&thread,NULL,monitor,NULL))goto fail;
 while(!__atomic_load_n(&ready,__ATOMIC_ACQUIRE))sched_yield();
 uint64_t start=ns();
 for(unsigned round=0;round<100;round++){
  /* Split one live interval into two nodes and merge it back. */
  size_t i=round%count;
  if(mprotect(p+i*8192,8192,PROT_READ|PROT_WRITE)||mprotect(p+i*8192+4096,4096,PROT_NONE))goto fail;
 }
 uint64_t elapsed=ns()-start;
 __atomic_store_n(&stop,1,__ATOMIC_RELAXED);pthread_join(thread,NULL);
 for(size_t i=0;i<count;i++)if(p[i*8192]!=(unsigned char)i)goto fail;
 printf("VMA_MERGE_LATENCY_BOOT: PASS mappings=%zu cycles=100 elapsed_us=%llu worst_wake_us=%llu\n",count*2,(unsigned long long)(elapsed/1000),(unsigned long long)(worst/1000));
 goto done;
 fail:puts("VMA_MERGE_LATENCY_BOOT: FAIL");
 done:fflush(stdout);reboot(RB_POWER_OFF);for(;;)pause();
}
'''
if __name__ == '__main__':
    run_probe('vma-merge-latency-boot', INIT, 'VMA_MERGE_LATENCY_BOOT: PASS',
              cpus=int(os.environ.get('AVORY_TEST_CPUS', '4')), timeout=180)
