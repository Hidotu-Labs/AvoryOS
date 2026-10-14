#!/usr/bin/env python3
"""Owned submission policy plus actual imported ACQUIRE_MEM emission; CPU only."""
from pathlib import Path
import re,subprocess,tempfile
ROOT=Path(__file__).resolve().parents[2]
AMD=ROOT/'kernel/linux/drivers/gpu/drm/amd/amdgpu'
s=(AMD/'gfx_v10_0.c').read_text();start=s.index('static void gfx_v10_0_emit_mem_sync(');end=s.index('\n}',start)+2
emitter=s[start:end]
macros='\n'.join(line for line in (AMD/'nvd.h').read_text().splitlines() if re.match(r'#define\s+PACKET3_ACQUIRE_MEM_GCR_CNTL_\w+\(x\)\s+',line))
sched=(AMD/'amdgpu_ib.c').read_text()
start=sched.index('if ((ib->flags & AMDGPU_IB_FLAG_EMIT_MEM_SYNC)');end=sched.index('\n\n',start)
request=sched[start:end]
for table in ('gfx_v10_0_ring_funcs_gfx','gfx_v10_0_ring_funcs_compute'):
 start=s.index('static const struct amdgpu_ring_funcs '+table);end=s.index('\n};',start)
 assert '8, /* gfx_v10_0_emit_mem_sync */' in s[start:end], 'imported frame budget no longer includes sync'
shim=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define AMD_IS_APU 1
#define GC_HWIP 0
#define IP_VERSION(a,b,c) (((a)<<16)|((b)<<8)|(c))
#define AMDGPU_RING_TYPE_GFX 0
#define AMDGPU_RING_TYPE_COMPUTE 1
#define AMDGPU_RING_TYPE_SDMA 2
#define AMDGPU_IB_FLAG_EMIT_MEM_SYNC (1u << 6)
#define PACKET3_ACQUIRE_MEM 0x58u
#define PACKET3(op,n) (0xc0000000u|((n)<<16)|((op)<<8))
#define module_param(a,b,c)
#define MODULE_PARM_DESC(a,b)
#define klogf(...) ((void)0)
struct pci_dev {unsigned vendor,device;};
struct amdgpu_device {struct pci_dev *pdev;unsigned flags;bool passthrough,vf,no_hw_access;unsigned ip_versions[1][1];};
struct amdgpu_ring;
struct amdgpu_ring_funcs {unsigned type;void (*emit_mem_sync)(struct amdgpu_ring*);};
struct amdgpu_ring {struct amdgpu_device *adev;const struct amdgpu_ring_funcs *funcs;const char *name;};
struct amdgpu_job {void *vm;};
struct amdgpu_ib {uint32_t flags;};
struct dma_fence {unsigned marker;};
#define amdgpu_passthrough(a) ((a)->passthrough)
#define amdgpu_sriov_vf(a) ((a)->vf)
static unsigned calls,writes;static int result;static uint32_t expected,packet[8];
static void amdgpu_ring_write(struct amdgpu_ring *r,uint32_t v){assert(r&&writes<8);packet[writes++]=v;}
'''
code=shim+macros+'\n'+emitter+'\n'
code+='''int linuxkpi_original_amdgpu_ib_schedule(struct amdgpu_ring *ring,unsigned num_ibs,struct amdgpu_ib *ibs,struct amdgpu_job *job,struct dma_fence **f){
 (void)job;calls++;assert(!num_ibs||ibs[0].flags==expected);
 if(num_ibs){struct amdgpu_ib *ib=&ibs[0];\n'''+request+'''}
 if(f){static struct dma_fence completion={73};*f=&completion;}return result;
}
'''
code+=re.sub(r'^#include.*$','',(ROOT/'kernel/linuxkpi/src/amdgpu_ib_visibility.c').read_text(),flags=re.M)
code+=r'''
int main(void){
 struct pci_dev pci={0x1002,0x164e};struct amdgpu_device base={.pdev=&pci,.flags=AMD_IS_APU,.passthrough=true,.ip_versions={{IP_VERSION(10,3,6)}}},adev=base;
 struct amdgpu_ring_funcs funcs={AMDGPU_RING_TYPE_GFX,gfx_v10_0_emit_mem_sync};struct amdgpu_ring r={&adev,&funcs,"gfx"};struct amdgpu_job job={(void*)1};struct amdgpu_ib ibs[2]={{1},{2}};struct dma_fence *f=NULL;
 assert(kpi_amdgpu_ib_mem_sync);
 for(unsigned type=0;type<2;type++)for(unsigned failure=0;failure<2;failure++){
  funcs.type=type;writes=0;result=failure?-12:0;expected=1|AMDGPU_IB_FLAG_EMIT_MEM_SYNC;unsigned before=calls;
  assert(amdgpu_ib_schedule(&r,2,ibs,&job,&f)==result&&calls==before+1);
  assert(ibs[0].flags==1&&ibs[1].flags==2&&f->marker==73&&writes==8);
  assert(packet[0]==PACKET3(0x58,6)&&packet[1]==0&&packet[2]==0xffffffff&&packet[3]==0xffffff&&packet[4]==0&&packet[5]==0&&packet[6]==10);
  assert(packet[7]==((1u<<0)|(1u<<4)|(1u<<5)|(1u<<7)|(1u<<8)|(1u<<9)|(1u<<14)|(1u<<15)));
 }
 result=0;
 for(unsigned exclusion=0;exclusion<13;exclusion++){
  adev=base;pci.vendor=0x1002;pci.device=0x164e;funcs.type=0;funcs.emit_mem_sync=gfx_v10_0_emit_mem_sync;kpi_amdgpu_ib_mem_sync=true;job.vm=(void*)1;
  struct amdgpu_job *jp=&job;unsigned n=2;
  switch(exclusion){case 0:kpi_amdgpu_ib_mem_sync=false;break;case 1:n=0;break;case 2:jp=NULL;break;case 3:job.vm=NULL;break;case 4:pci.vendor=0xffff;break;case 5:pci.device=0xffff;break;case 6:adev.passthrough=false;break;case 7:adev.vf=true;break;case 8:adev.ip_versions[0][0]=IP_VERSION(10,3,5);break;case 9:funcs.type=AMDGPU_RING_TYPE_SDMA;break;case 10:funcs.emit_mem_sync=NULL;break;case 11:adev.no_hw_access=true;break;case 12:adev.flags=0;break;}
  writes=0;expected=1;unsigned before=calls;assert(!amdgpu_ib_schedule(&r,n,ibs,jp,&f));assert(calls==before+1&&!writes&&ibs[0].flags==1);
 }
 puts("PASS: scoped submission sync, explicit disable, original flags/errors/fence preserved, one delegation, gfx+compute actual 8-dword full-cache packet and existing frame budget (CPU only)");
}
'''
with tempfile.TemporaryDirectory() as td:
 p=Path(td);(p/'test.c').write_text(code)
 subprocess.run(['cc','-std=gnu11','-Wall','-Wextra','-Werror','-O1','-g','-DAVORYOS_AMDGPU_MEMORY_POLICY','-fsanitize=address,undefined',p/'test.c','-o',p/'test'],check=True)
 subprocess.run([p/'test'],check=True)
 subprocess.run(['cc','-nostdinc','-c',ROOT/'kernel/linuxkpi/src/amdgpu_ib_visibility.c','-o',p/'disabled.o'],check=True)
