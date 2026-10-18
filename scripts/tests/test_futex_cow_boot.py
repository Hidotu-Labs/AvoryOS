#!/usr/bin/env python3
"""A private futex without PRIVATE must wake after its page is replaced."""
from boot_probe import run_probe
INIT = r'''
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/reboot.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>
static uint32_t *word;
static unsigned ready;
static long waited;
static int wait_errno;
static void *waiter(void *arg) {
 (void)arg;struct timespec timeout={5,0};
 __atomic_store_n(&ready,1,__ATOMIC_RELEASE);
 waited=syscall(SYS_futex,word,0,7,&timeout,NULL,0);wait_errno=errno;
 return NULL;
}
int main(void) {
 setbuf(stdout,NULL);puts("FUTEX_COW_BOOT: START");
 mount("proc","/proc","proc",0,NULL);
 int ctl=open("/proc/sys/vm/ksm_run",O_WRONLY);
 if(ctl>=0){write(ctl,"0",1);close(ctl);}
 word=mmap(NULL,4096,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
 if(word==MAP_FAILED){puts("FUTEX_COW_BOOT: FAIL mmap");goto done;}
 *word=7;
 pthread_t worker;
 if(pthread_create(&worker,NULL,waiter,NULL)){puts("FUTEX_COW_BOOT: FAIL thread");goto done;}
 while(!__atomic_load_n(&ready,__ATOMIC_ACQUIRE))usleep(1000);
 usleep(100000);
 /* Discard the allocated anonymous page and fault in the shared zero page.
  * The VMA/address are stable, while the old physical wait key is invalid. */
 int discard=madvise(word,4096,MADV_DONTNEED);
 volatile uint32_t current=*word;
 long woke=syscall(SYS_futex,word,1,1,NULL,NULL,0);
 pthread_join(worker,NULL);
 printf("FUTEX_COW_BOOT: discard=%d value=%u woke=%ld waited=%ld errno=%d\n",discard,current,woke,waited,wait_errno);
 puts(!discard && current==0 && woke==1 && waited==0?"FUTEX_COW_BOOT: PASS":"FUTEX_COW_BOOT: FAIL");
 done:
 fflush(stdout);reboot(RB_POWER_OFF);for(;;)pause();
}
'''
if __name__=='__main__':
    run_probe('futex-cow-boot',INIT,'FUTEX_COW_BOOT: PASS',cpus=4)
