#!/usr/bin/env python3
"""Fault-inject actual LinuxKPI PRIME mapping setup and check cleanup/errors."""
from pathlib import Path
import re
import subprocess
import tempfile

ROOT=Path(__file__).resolve().parents[2]
source=(ROOT/'kernel/linuxkpi/src/native_prime.c').read_text()
mapping=source[source.index('struct native_prime_mapping {'):source.index('\nstatic struct sg_table *native_map')]
code=source[source.index('static struct device prime_device;'):source.index('\nunsigned long linuxkpi_prime_page_phys')]
pre=r'''
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <linuxkpi/gpu_diag.h>
#define GFP_KERNEL 0
#define DMA_BIDIRECTIONAL 0
#define DMA_BIT_MASK(n) UINT64_MAX
#define ERR_PTR(e) ((void*)(intptr_t)(e))
#define IS_ERR(p) ((uintptr_t)(p)>=(uintptr_t)-4095)
#define PTR_ERR(p) ((int)(intptr_t)(p))
typedef uint64_t u64;
struct mutex {bool held;};
#define DEFINE_MUTEX(n) struct mutex n
static void mutex_init(struct mutex *m){m->held=false;}
static void mutex_lock(struct mutex *m){assert(!m->held);m->held=true;}
static void mutex_unlock(struct mutex *m){assert(m->held);m->held=false;}
struct device {u64 dma_mask,coherent_dma_mask;};
struct dma_buf {size_t size;};
struct dma_buf_attachment {struct dma_buf *dmabuf;};
struct sg_table {int unused;};
struct iosys_map {void *vaddr;bool is_iomem;};
static void device_initialize(struct device *d){(void)d;}
static void dev_set_name(struct device *d,const char *n){(void)d;(void)n;}
static int fault,stage,errors,attach_count,detach_count,sg_count,unmap_count,vmap_count,vunmap_count,alloc_count;
static unsigned char pixels[8192];
static struct dma_buf_attachment attach;
static struct sg_table sgt;
static void *kzalloc(size_t n,int flags){(void)flags;if(fault==1)return NULL;alloc_count++;return calloc(1,n);}
static void kfree(void *p){assert(p);alloc_count--;free(p);}
static struct dma_buf_attachment *dma_buf_attach(struct dma_buf *b,struct device *d){
 (void)d;if(fault==2)return ERR_PTR(-EBUSY);attach.dmabuf=b;attach_count++;return &attach;
}
static struct sg_table *dma_buf_map_attachment_unlocked(struct dma_buf_attachment *a,int dir){
 (void)dir;assert(a==&attach);if(fault==3)return ERR_PTR(-EINVAL);sg_count++;return &sgt;
}
static int dma_buf_vmap_unlocked(struct dma_buf *b,struct iosys_map *m){
 (void)b;if(fault==4)return -EIO;m->vaddr=pixels;m->is_iomem=fault==5;vmap_count++;return 0;
}
static void dma_buf_vunmap_unlocked(struct dma_buf *b,struct iosys_map *m){
 (void)b;assert(m->vaddr==pixels);vunmap_count++;
}
static void dma_buf_unmap_attachment_unlocked(struct dma_buf_attachment *a,struct sg_table *s,int dir){
 (void)dir;assert(a==&attach && s==&sgt);unmap_count++;
}
static void dma_buf_detach(struct dma_buf *b,struct dma_buf_attachment *a){assert(a==&attach && a->dmabuf==b);detach_count++;}
void gpu_diag_prime_error(unsigned int s,const void *o,uint64_t detail,uint64_t size,long result){
 assert(o==attach.dmabuf && detail==0 && size==8192 && result<0);stage=s;errors++;
}
'''
post=r'''
int main(void){
 struct dma_buf b={.size=8192};
 const int expected_stage[]={0,GPU_PRIME_OBJECT_ALLOC,GPU_PRIME_ATTACH,GPU_PRIME_SG_MAP,GPU_PRIME_CPU_MAP,GPU_PRIME_IOMEM};
 const int expected_ret[]={0,-ENOMEM,-EBUSY,-EINVAL,-EIO,-EOPNOTSUPP};
 for(fault=0;fault<=5;fault++){
  errors=stage=attach_count=detach_count=sg_count=unmap_count=vmap_count=vunmap_count=alloc_count=0;
  attach.dmabuf=&b;void *m=NULL,*address=NULL;
  int ret=linuxkpi_prime_map(&b,&m,&address);assert(ret==expected_ret[fault]);
  if(!fault){assert(m && address==pixels && !errors);linuxkpi_prime_unmap(m);}
  else{assert(!m && !address && errors==1 && stage==expected_stage[fault]);}
  assert(attach_count==detach_count && sg_count==unmap_count && vmap_count==vunmap_count && !alloc_count);
  assert(!prime_device_lock.held);
 }
 puts("PASS: actual PRIME setup reports allocation/attach/SG/vmap/IOMEM errors once and balances cleanup; normal import unchanged");
}
'''
with tempfile.TemporaryDirectory(prefix='avory-prime-errors-') as temp:
    p=Path(temp);(p/'check.c').write_text(pre+mapping+code+post)
    subprocess.run(['cc','-O1','-g','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',
        '-I'+str(ROOT/'kernel/linuxkpi/include'),p/'check.c','-o',p/'check'],check=True)
    subprocess.run([p/'check'],check=True,timeout=10)
