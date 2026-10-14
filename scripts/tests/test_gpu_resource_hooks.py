#!/usr/bin/env python3
"""Run actual AMDGPU trace wrappers: forwarding, identities and signal lifetime."""
from pathlib import Path
import re,subprocess,tempfile
root=Path(__file__).resolve().parents[2]
s=(root/'kernel/linuxkpi/src/amdgpu_trace.c').read_text();s=re.sub(r'^#include.*$','',s,flags=re.M)
shim=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <linuxkpi/gpu_diag.h>
typedef uint64_t u64;
struct drm_device{int dummy;};struct drm_file{int dummy;};
struct dma_fence{u64 context,seqno;};struct dma_resv{int dummy;};
enum dma_resv_usage{DMA_RESV_USAGE_KERNEL,DMA_RESV_USAGE_WRITE};
struct page{u64 phys;int _refcount;};
struct ttm_tt{struct page **pages;u64 *dma_address;unsigned long num_pages;};
struct ttm_resource{unsigned mem_type;};
struct amdgpu_device{int dummy;};
struct psp_context{struct amdgpu_device *adev;};
static int psp_result;static unsigned psp_calls,status_reads,diagnostics;
static uint32_t psp_read(uint32_t reg){assert(reg==0x40);status_reads++;return 0xdead;}
#define RREG32(reg) ((void)adev,psp_read(reg))
static void klogf(const char*fmt,...){
 assert(fmt);va_list a;va_start(a,fmt);
 assert(va_arg(a,unsigned)==0x40);assert(va_arg(a,unsigned)==0x80000000);
 assert(va_arg(a,unsigned)==0x8000ffff);assert(va_arg(a,unsigned)==0xdead);
 assert(va_arg(a,int)==0);assert(va_arg(a,int)==-62);va_end(a);diagnostics++;
}
int linuxkpi_original_psp_wait_for(struct psp_context*p,uint32_t reg,uint32_t val,uint32_t mask,bool changed){assert(p&&reg==0x40&&val==0x80000000&&mask==0x8000ffff&&!changed);psp_calls++;return psp_result;}
struct amdgpu_ring{int dummy;};struct amdgpu_job{int dummy;};
struct amdgpu_bo{struct{struct{u64 size;struct dma_resv *resv;}base;struct ttm_tt *ttm;struct ttm_resource *resource;}tbo;};
struct amdgpu_bo_va{struct{struct amdgpu_bo *bo;void *vm;}base;struct dma_fence *last_pt_update;};
struct drm_amdgpu_gem_va{uint32_t handle,operation;u64 va_address,map_size,flags,offset_in_bo;};
#define atomic_read(p) (*(p))
static u64 page_to_phys(struct page*p){return p->phys;}
static bool enabled=true;int gpu_diag_resource_enabled(void){return enabled;}
static unsigned calls,events;static unsigned kind[32];static u64 values[32][4];static const void *objects[32];
void gpu_diag_record(unsigned k,const void*o,u64 a,u64 b,u64 c,u64 d,long result){(void)result;assert(events<32);kind[events]=k;objects[events]=o;values[events][0]=a;values[events][1]=b;values[events][2]=c;values[events++][3]=d;}
int linuxkpi_original_amdgpu_gem_va_ioctl(struct drm_device*d,void*p,struct drm_file*f){assert(d&&p&&f);calls++;return -5;}
int linuxkpi_original_amdgpu_vm_bo_update(struct amdgpu_device*d,struct amdgpu_bo_va*v,bool clear){assert(d&&v&&clear);calls++;return -12;}
int linuxkpi_original_amdgpu_fence_emit(struct amdgpu_ring*r,struct dma_fence **f,struct amdgpu_job*j,unsigned flags){assert(r&&j&&flags==7);calls++;*f=malloc(sizeof(**f));assert(*f);**f=(struct dma_fence){19,23};return 0;}
int linuxkpi_original_dma_fence_signal(struct dma_fence*f){calls++;if(!f)return -22;assert(f->context==19&&f->seqno==23);free(f);return 0;}
void linuxkpi_original_dma_resv_add_fence(struct dma_resv*r,struct dma_fence*f,enum dma_resv_usage u){assert(r&&f&&u==DMA_RESV_USAGE_WRITE);calls++;}
'''
main=r'''
int main(void){
 struct drm_device dev;struct drm_file file;struct drm_amdgpu_gem_va args={.handle=5,.operation=1,.va_address=0x800000,.map_size=8192,.flags=7};
 assert(amdgpu_gem_va_ioctl(&dev,&args,&file)==-5&&calls==1&&events==2);
 assert(kind[0]==GPU_DIAG_VM_BIND&&values[0][0]==((u64)5<<32|1)&&values[0][1]==0x800000);
 struct page p[2]={{0x100000000ULL,1},{0x100001000ULL,2}};struct page *pages[]={p,p+1};u64 dma[]={p[0].phys,p[1].phys};
 struct ttm_tt tt={pages,dma,2};struct ttm_resource resource={1};struct dma_resv resv;
 struct amdgpu_bo bo={.tbo={.base={8192,&resv},.ttm=&tt,.resource=&resource}};
 struct amdgpu_bo_va va={.base={&bo,(void*)0x1234}};struct amdgpu_device adev;
 assert(amdgpu_vm_bo_update(&adev,&va,true)==-12&&calls==2&&events==6);
 assert(kind[2]==GPU_DIAG_BO_BACKING&&objects[2]==&bo);
 assert(kind[3]==GPU_DIAG_PAGE_BIND&&values[3][1]==p[0].phys&&values[3][2]==dma[0]);
 assert(kind[4]==GPU_DIAG_PAGE_BIND&&values[4][3]==2&&kind[5]==GPU_DIAG_VM_UPDATE);
 struct amdgpu_ring ring;struct amdgpu_job job;struct dma_fence *f;
 assert(!amdgpu_fence_emit(&ring,&f,&job,7));assert(kind[6]==GPU_DIAG_FENCE_EMIT);
 dma_resv_add_fence(&resv,f,DMA_RESV_USAGE_WRITE);assert(kind[7]==GPU_DIAG_RESV_FENCE);
 assert(!dma_fence_signal(f));assert(kind[8]==GPU_DIAG_FENCE_SIGNAL&&values[8][0]==19&&values[8][1]==23&&values[8][2]==0);
 assert(kind[9]==GPU_DIAG_FENCE_SIGNAL&&values[9][2]==1);
 assert(dma_fence_signal(NULL)==-22&&events==10&&calls==6);
 enabled=false;assert(amdgpu_gem_va_ioctl(&dev,&args,&file)==-5);assert(events==10&&calls==7);
 struct psp_context psp={&adev};
 assert(!psp_wait_for(&psp,0x40,0x80000000,0x8000ffff,false)&&psp_calls==1&&!status_reads&&!diagnostics);
 psp_result=-62;assert(psp_wait_for(&psp,0x40,0x80000000,0x8000ffff,false)==-62);
 assert(psp_calls==2&&status_reads==1&&diagnostics==1);
 puts("PASS: PSP wait failures preserve original result and report status with one read; VM/BO/DMA/fence identities, original failures forwarded, optional tracing disabled, no post-signal dereference under ASan/UBSan");
}
'''
with tempfile.TemporaryDirectory(prefix='gpu-resource-hooks-') as tmp:
 p=Path(tmp);(p/'test.c').write_text(shim+s+main)
 subprocess.run(['cc','-std=gnu11','-O1','-g','-DAVORYOS_AMDGPU_MEMORY_POLICY','-fsanitize=address,undefined','-I'+str(root/'kernel/linuxkpi/include'),str(p/'test.c'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True,timeout=10)
