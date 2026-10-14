#!/usr/bin/env python3
"""Run the actual recorder: concurrency, wrap, freeze, chunked reads and hashes."""
from pathlib import Path
import re, subprocess, tempfile
root=Path(__file__).resolve().parents[2]
source=(root/'kernel/src/linuxkpi/gpu_diag.c').read_text()
source=re.sub(r'^#include.*\n','',source,flags=re.M)
code=r'''
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <pthread.h>
#include <stdarg.h>
#include <linuxkpi/gpu_diag.h>
typedef pthread_mutex_t spinlock_t;
static void spinlock_acquire(spinlock_t *p){assert(!pthread_mutex_lock(p));}
static void spinlock_release(spinlock_t *p){assert(!pthread_mutex_unlock(p));}
struct thread {unsigned int tid;};
static _Thread_local struct thread thread={.tid=12};
static struct thread *sched_get_current(void){return &thread;}
static uint64_t clock_ms=17;
static uint64_t lapic_timer_get_ms(void){return clock_ms;}
static void lapic_timer_sleep(uint32_t ms){clock_ms+=ms;}
static char serial_text[512*1024];
static unsigned serial_pending,serial_size,serial_dropped;
static bool serial_stalled;
static unsigned serial_pending_bytes(void){return serial_pending;}
static void serial_flush_idle(void){if(!serial_stalled)serial_pending=serial_pending>2048?serial_pending-2048:0;}
static void klog_puts(const char *p){
 unsigned len=strlen(p),space=131071-serial_pending;
 assert(len && p[len-1]=='\n'); /* dumps queue complete lines */
 if(len>space){serial_dropped+=len-space;len=space;}
 assert(serial_size+len<sizeof(serial_text));
 memcpy(serial_text+serial_size,p,len);serial_size+=len;
 serial_text[serial_size]=0;serial_pending+=len;
}
static void klogf(const char *format,...){
 char line[280];va_list args;va_start(args,format);vsnprintf(line,sizeof line,format,args);va_end(args);klog_puts(line);
}
static uint64_t *vmm_get_kernel_pml4(void){return (void*)1;}
static uint64_t pmm_get_hhdm_offset(void){return 0;}
static uint64_t vmm_debug_cache_flags(uint64_t *p,uint64_t a){(void)p;(void)a;return 0;}
'''+source+r'''
static char text[512*1024],small[512*1024];
static void command(const char *s){assert(gpu_diag_control(strlen(s),(void*)s)==strlen(s));}
static void *writer(void *arg){thread.tid=(uintptr_t)arg;for(int i=0;i<1000;i++)gpu_diag_record(GPU_DIAG_IOCTL,(void*)0xffff800000001900ULL,0xffffc00000000000ULL+i,0x7fffeffff000ULL,0x8000000000000006ULL,0xffff800012345678ULL,0);return NULL;}
int main(void){
 command("1\n");
 gpu_diag_record(GPU_DIAG_DMA_MAP,NULL,1,1,4096,0,0);assert(resource_count==0);
 command("3");assert(gpu_diag_resource_enabled());
 for(int i=0;i<2000;i++)gpu_diag_record(GPU_DIAG_DMA_MAP,(void*)(uintptr_t)i,i,i,4096,0,0);
 assert(resource_count==1024&&!resource_left&&!gpu_diag_resource_enabled());
 for(int i=0;i<3000;i++)gpu_diag_record(GPU_DIAG_IOCTL,NULL,0,0,0,0,0);
 command("freeze");unsigned rn=gpu_diag_read(0,sizeof(text)-1,(void*)text);text[rn]=0;
 assert(strstr(text,"resource_events=1024 resource_budget_left=0")&&strstr(text,"retained resource events")&&strstr(text,"DMA_MAP"));
 command("1\n");
 for(int i=0;i<18;i++)gpu_diag_prime_error(GPU_PRIME_CPU_MAP,NULL,0,4096,-12);
 assert(prime_error_lines==18 && strstr(serial_text,"PRIME stage=7 detail=0x0 size=4096 error=-12 tid=12"));
 unsigned lines=0;char *cursor=serial_text;
 while((cursor=strstr(cursor,"[GPUDBG] PRIME stage="))){lines++;cursor++;}
 assert(lines==16);
 for(int i=0;i<3000;i++)gpu_diag_record(GPU_DIAG_IOCTL,NULL,0,0,0,0,0);
 command("freeze");unsigned errn=gpu_diag_read(0,sizeof(text)-1,(void*)text);text[errn]=0;
 assert(strstr(text,"anomalies=18") && strstr(text,"IMPORT") && strstr(text," -12\n"));
 unsigned error_serial_size=serial_size;gpu_diag_prime_error(GPU_PRIME_CPU_MAP,NULL,0,4096,-12);assert(serial_size==error_serial_size);
 command("1\n");pthread_t threads[8];
 for(int i=0;i<8;i++)pthread_create(&threads[i],NULL,writer,(void*)(uintptr_t)i);
 for(int i=0;i<8;i++)pthread_join(threads[i],NULL);
 command("freeze\n");unsigned n=gpu_diag_read(0,sizeof(text)-1,(void*)text);text[n]=0;
 assert(strstr(text,"events=8000 overwritten=5952"));
 assert(strstr(text,"count IOCTL=8000"));assert(!strstr(text,"overwritten while reading"));
 unsigned pos=0,read;
 while((read=gpu_diag_read(pos,37,(void*)(small+pos))))pos+=read;
 assert(pos==n && !memcmp(text,small,n));
 command("dump");assert(!serial_dropped && !serial_pending);
 assert(serial_size>131072 && strstr(serial_text,"[GPUDBG] frozen flight recorder end"));
 const char *begin=strstr(serial_text,"GPUDBG v1");
 assert(begin && !memcmp(begin,text,n)); /* exact full frozen report, no chunk loss */
 serial_size=serial_pending=0;
 gpu_diag_record(GPU_DIAG_IOCTL,NULL,0,0,0,0,0);assert(sequence==8000);
 command("2");for(int i=0;i<64;i++)assert(gpu_diag_take_sample());assert(!gpu_diag_take_sample());
 unsigned char padded[3][12]={{1,2,3,4,5,6,7,8},{9,10,11,12,13,14,15,16},{17,18,19,20,21,22,23,24}};
 unsigned char packed[24];for(int i=0;i<3;i++)memcpy(packed+i*8,padded[i],8);
 uint64_t h=gpu_diag_hash_rect(padded,8,3,12);
 assert(h==gpu_diag_hash_rect(packed,8,3,8));padded[1][11]=55;assert(h==gpu_diag_hash_rect(padded,8,3,12));
 packed[0]^=1;assert(h!=gpu_diag_hash_rect(packed,8,3,8));
 gpu_diag_record(GPU_DIAG_COPY_CHECK,NULL,1,2,3,4,1);
 gpu_diag_record(GPU_DIAG_COPY_CHECK,NULL,1,2,3,4,2);
 command("dump\n");n=gpu_diag_read(0,sizeof(text)-1,(void*)text);text[n]=0;
 assert(strstr(text,"source_changed=1 stable_copy_mismatch=1"));
 command("1");
 for(int i=0;i<64;i++)gpu_diag_record(GPU_DIAG_COPY_CHECK,NULL,i,22,22,22,0);
 for(int i=0;i<3000;i++)gpu_diag_record(GPU_DIAG_IOCTL,NULL,0,0,0,0,0);
 command("freeze");n=gpu_diag_read(0,sizeof(text)-1,(void*)text);text[n]=0;
 unsigned retained=0;char *next=text;
 while((next=strstr(next,"sample "))){retained++;next++;}
 assert(retained==64 && strstr(text,"count COPY_CHECK=64"));
 clock_ms=17;
 command("1");gpu_diag_cache_check((void*)22,4096,128);
 for(int i=0;i<3000;i++)gpu_diag_record(GPU_DIAG_IOCTL,NULL,0,0,0,0,0);
 command("freeze");n=gpu_diag_read(0,sizeof(text)-1,(void*)text);text[n]=0;
 assert(strstr(text,"cache_mismatch=1 anomalies=1"));assert(strstr(text,"anomaly 1 17 12 CACHE_CHECK"));
 serial_size=0;serial_pending=9000;serial_stalled=true;
 uint64_t before=clock_ms;command("dump");
 assert(clock_ms-before==30000 && strstr(serial_text,"serial export timed out"));
 assert(!strstr(serial_text,"frozen flight recorder end"));
 n=gpu_diag_read(0,sizeof(text)-1,(void*)text);text[n]=0;
 assert(strstr(text,"cache_mismatch=1 anomalies=1"));
 assert(!gpu_diag_control(3,(void*)"bad"));
 command("clear\n");assert(!sequence && !gpu_diag_level());
 puts("PASS: GPU recorder concurrency, retained samples, 128 KiB serial backpressure, bounded stalled export, chunked reads and pixel hashes");
}
'''
with tempfile.TemporaryDirectory(prefix='avory-gpu-diag-') as temp:
    p=Path(temp);(p/'check.c').write_text(code)
    subprocess.run(['cc','-O2','-pthread','-Wall','-Wextra','-Werror','-I'+str(root/'kernel/linuxkpi/include'),str(p/'check.c'),'-o',str(p/'check')],check=True)
    subprocess.run([str(p/'check')],check=True,timeout=15)
