#!/usr/bin/env python3
"""Exercise opt-in timing without a GPU, plus concurrent wall-time accounting."""
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
source = (ROOT/'kernel/src/linuxkpi/gpu_perf.c').read_text()
source = re.sub(r'^#include.*$', '', source, flags=re.M)
shim = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <pthread.h>
static _Thread_local uint64_t clock_ns=1000;
static uint64_t lapic_timer_get_ns(void){return ++clock_ns;}
static unsigned disabled;
static unsigned gpu_diag_control(unsigned size,const unsigned char *data){assert(size==1&&*data=='0');disabled++;return size;}
struct virtio_gpu_stats{uint64_t frames_presented,damage_pixels,scanout_pixels,unchanged_frames,producer_sync_ns,snapshot_copy_ns,transport_wait_ns;};
static void virtio_gpu_get_stats(struct virtio_gpu_stats *stats){memset(stats,0,sizeof *stats);}
#define klogf printf
#define klog_puts puts
'''
test = r'''
static void *worker(void *unused){
 (void)unused;
 for(unsigned sample=0;sample<1000;sample++){
  uint64_t start=gpu_perf_ioctl_start(0xc0186452);
  clock_ns+=9;
  gpu_perf_ioctl_finish(0xc0186452,start,sample%2?-1:0);
 }
 return NULL;
}
int main(void){
 gpu_perf_init(NULL);assert(!gpu_perf_ioctl_start(0x6452)&&!disabled);
 gpu_perf_init("not_gpu_perf=1 gpu_perf=10 not_gpu_diag=0 gpu_diag=01");
 assert(!gpu_perf_ioctl_start(0x6452)&&!disabled);
 gpu_perf_init("gpu_perf=1 gpu_diag=0");assert(disabled==1);
 assert(!gpu_perf_ioctl_start(0x7852));
 pthread_t threads[8];
 for(unsigned index=0;index<8;index++)assert(!pthread_create(&threads[index],NULL,worker,NULL));
 for(unsigned index=0;index<8;index++)assert(!pthread_join(threads[index],NULL));
 assert(ioctl_costs[0x52].calls==8000&&ioctl_costs[0x52].errors==4000);
 assert(ioctl_costs[0x52].total_ns==80000&&ioctl_costs[0x52].max_ns==10);
 gpu_perf_init("gpu_perf=0");assert(!gpu_perf_ioctl_start(0x6452));
 gpu_perf_ioctl_finish(0x6452,0,0);assert(ioctl_costs[0x52].calls==8000);
 gpu_perf_dump();
 puts("PASS: exact boot options, disabled fast path, DRM filtering and 8000 concurrent ioctl wall-time samples");
}
'''
with tempfile.TemporaryDirectory(prefix='avory-gpu-perf-') as temporary:
    directory = Path(temporary)
    (directory/'test.c').write_text(shim+source+test)
    subprocess.run(['cc','-O2','-pthread','-Wall','-Wextra','-Werror',
                    str(directory/'test.c'),'-o',str(directory/'test')],check=True)
    subprocess.run([str(directory/'test')],check=True,timeout=10)
