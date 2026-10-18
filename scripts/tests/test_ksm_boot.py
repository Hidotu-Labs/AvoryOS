#!/usr/bin/env python3
"""Exercise KSM alongside private-page writers in a disposable 4-vCPU VM."""
from boot_probe import run_probe

INIT = r'''
#define _GNU_SOURCE
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/reboot.h>
#include <unistd.h>
#define N 256
static volatile unsigned stop, failures;
static volatile uint32_t *pages[3];
static void *writer(void *arg) {
 unsigned id=(uintptr_t)arg;
 uint32_t value=0x12340000+id;
 while(!__atomic_load_n(&stop,__ATOMIC_ACQUIRE)) {
  for(unsigned i=0;i<N;i++) {
   pages[id][i*1024]=value;
   pages[id][i*1024+1023]=~value;
  }
  for(unsigned i=0;i<N;i++)
   if(pages[id][i*1024]!=value || pages[id][i*1024+1023]!=~value)
    __atomic_add_fetch(&failures,1,__ATOMIC_RELAXED);
 }
 return NULL;
}
int main(void) {
 setbuf(stdout,NULL); puts("KSM_BOOT: START");
 mount("proc","/proc","proc",0,NULL);
 int fd=open("/proc/sys/vm/ksm_run",O_WRONLY);
 if(fd<0) { puts("KSM_BOOT: FAIL no control"); goto done; }
 pthread_t workers[3];
 for(unsigned i=0;i<3;i++) {
  pages[i]=mmap(NULL,N*4096,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
  if(pages[i]==MAP_FAILED || pthread_create(&workers[i],NULL,writer,(void *)(uintptr_t)i)) {
   puts("KSM_BOOT: FAIL worker");goto done;
  }
 }
 unsigned char *same=mmap(NULL,2*N*4096,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
 if(same==MAP_FAILED) { puts("KSM_BOOT: FAIL mmap");goto done; }
 memset(same,0x5a,N*4096);memset(same+N*4096,0,N*4096);
 for(unsigned i=0;i<16;i++) {
  if(write(fd,"1",1)!=1) { failures++;break; }
  usleep(10000);
 }
 __atomic_store_n(&stop,1,__ATOMIC_RELEASE);
 for(unsigned i=0;i<3;i++) pthread_join(workers[i],NULL);
 for(unsigned i=0;i<2*N*4096;i++)
  if(same[i]!=(i<N*4096?0x5a:0)) { failures++;break; }
 /* Break every merged mapping and verify independent private writes. */
 for(unsigned i=0;i<2*N;i++) same[i*4096]=(unsigned char)i;
 for(unsigned i=0;i<2*N;i++) if(same[i*4096]!=(unsigned char)i) failures++;
 char stat[64]={0};int st=open("/proc/sys/vm/ksm_zero_pages_merged",O_RDONLY);
 if(st>=0) { read(st,stat,sizeof(stat)-1);close(st); }
 printf("KSM_BOOT: zero merges=%s",stat);
 if(strtoull(stat,NULL,10)==0) failures++;
 printf("KSM_BOOT: %s failures=%u\n",failures?"FAIL":"PASS",failures);
 done:
 fflush(stdout);sync();reboot(RB_POWER_OFF);for(;;)pause();
}
'''
if __name__ == '__main__':
    out = run_probe('ksm-boot', INIT, 'KSM_BOOT: PASS', cpus=4, timeout=90)
    log = (out/'serial.log').read_text(errors='replace')
    for bad in ('[PMM] CORRUPTION', '[PMM] REF ERROR', '[KPF]', 'FATAL:'):
        if bad in log:
            raise RuntimeError(f'{bad} in {out}/serial.log')
