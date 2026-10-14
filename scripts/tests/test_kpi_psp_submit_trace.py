#!/usr/bin/env python3
"""CPU-only PSP publication tracing and TMR address regression. No hardware I/O."""
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
PSP = ROOT / 'kernel/linux/drivers/gpu/drm/amd/amdgpu/amdgpu_psp.c'


def function(name):
    source = PSP.read_text()
    match = re.search(r'^(?:static )?(?:int|void)\s+' + name + r'\([^;]*?\)\s*\{', source, re.M)
    assert match
    end = source.index('{', match.start()) + 1
    depth = 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[match.start():end]


SHIM = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#define atomic_read(p) (*(p))
#define lower_32_bits(v) ((uint32_t)(v))
#define upper_32_bits(v) ((uint32_t)((v)>>32))
#define GFX_CMD_ID_SETUP_TMR 5
#define GFX_CMD_ID_SETUP_VMR 9
#define GFX_CMD_ID_LOAD_IP_FW 6
#define amdgpu_sriov_vf(a) ((a)->vf)
#define EINVAL 22
#define dev_err(...) ((void)0)
struct amdgpu_device {
 bool accel_working,vf;
 struct { uint64_t vram_start,vram_end; } gmc;
 struct { uint64_t vram_base_offset; } vm_manager;
};
struct amdgpu_bo { uint64_t size,gpu; };
static uint64_t amdgpu_bo_size(struct amdgpu_bo *b) { return b->size; }
static uint64_t amdgpu_gmc_vram_pa(struct amdgpu_device *a,struct amdgpu_bo *b) {
 return b->gpu-a->gmc.vram_start+a->vm_manager.vram_base_offset;
}
struct psp_gfx_cmd_setup_tmr { uint32_t buf_phy_addr_lo,buf_phy_addr_hi,buf_size;
 union { struct { uint32_t sriov_enabled:1,virt_phy_addr:1,reserved:30; } bitfield; uint32_t tmr_flags; };
 uint32_t system_phy_addr_lo,system_phy_addr_hi;
};
struct psp_gfx_cmd_load_ip_fw { uint32_t fw_phy_addr_lo,fw_phy_addr_hi,fw_size,fw_type; };
struct psp_gfx_cmd_resp { uint32_t cmd_id; union { struct psp_gfx_cmd_setup_tmr cmd_setup_tmr; struct psp_gfx_cmd_load_ip_fw cmd_load_ip_fw; } cmd; };
struct psp_gfx_rb_frame { uint32_t cmd_buf_addr_lo,cmd_buf_addr_hi,cmd_buf_size;
 uint32_t fence_addr_lo,fence_addr_hi,fence_value,sid_lo,sid_hi;
 uint8_t vmid,frame_type,reserved1[2]; uint32_t reserved2[7]; };
struct psp_ring { struct psp_gfx_rb_frame *ring_mem; unsigned ring_size; };
struct psp_context;
enum psp_ring_type { PSP_RING_TYPE__KM=2 };
struct psp_funcs { void (*ring_set_wptr)(struct psp_context*,uint32_t); };
struct psp_context { struct amdgpu_device *adev; struct psp_gfx_cmd_resp *cmd_buf_mem;
 uint64_t cmd_buf_mc_addr,fence_buf_mc_addr; int fence_value;
 const struct psp_funcs *funcs; struct psp_ring km_ring; };
static unsigned old_wptr,calls,flushes,logs,expected_before;
static char records[8][512];
static void klogf(const char *fmt,...) {
 assert(logs<8); va_list args; va_start(args,fmt);
 vsnprintf(records[logs++],512,fmt,args); va_end(args);
}
#define psp_ring_get_wptr(p) ((void)(p),old_wptr)
static void amdgpu_device_flush_hdp(struct amdgpu_device *a,void *ring) { assert(a&&!ring);flushes++; }
static void publish(struct psp_context *p,uint32_t value) {
 assert(flushes==1 && logs==expected_before); calls++;
 unsigned n=old_wptr/16; struct psp_gfx_rb_frame *f=&p->km_ring.ring_mem[n];
 assert(f->cmd_buf_addr_lo==0x220000 && f->cmd_buf_addr_hi==0xf4);
 assert(f->fence_addr_lo==0x230000 && f->fence_addr_hi==0xf4 && f->fence_value==(unsigned)p->fence_value);
 assert(value==(old_wptr+16)%1024);
}
'''
MAIN = r'''
int main(void) {
 struct amdgpu_device a={.gmc={0xf400000000ULL,0xf41fffffffULL},.vm_manager={0x80000000}};
 struct amdgpu_bo bo={0xa00000,0xf41e000000ULL};
 struct psp_gfx_cmd_resp cmd={0}; struct psp_gfx_rb_frame frames[64];
 const struct psp_funcs funcs={publish};
 struct psp_context p={&a,&cmd,0xf400220000ULL,0xf400230000ULL,1,&funcs,{frames,4096}};
 _Static_assert(sizeof(frames[0])==64,"frame ABI");
 psp_prep_tmr_cmd_buf(&p,&cmd,bo.gpu,&bo);
 assert(cmd.cmd_id==5 && cmd.cmd.cmd_setup_tmr.buf_phy_addr_hi==0xf4);
 assert(cmd.cmd.cmd_setup_tmr.system_phy_addr_lo==0x9e000000 && cmd.cmd.cmd_setup_tmr.system_phy_addr_hi==0);
 assert(cmd.cmd.cmd_setup_tmr.tmr_flags==2 && cmd.cmd.cmd_setup_tmr.buf_size==0xa00000);
 for(unsigned scenario=0;scenario<7;scenario++) {
  logs=calls=flushes=0; memset(frames,0xa5,sizeof(frames));
  old_wptr=scenario==1?1008:0; a.accel_working=scenario==2;
  p.fence_value=scenario==3?129:1;
  cmd.cmd_id=scenario==4?0x21:scenario==5?9:scenario==6?6:5;
  if(scenario==6) cmd.cmd.cmd_load_ip_fw=(struct psp_gfx_cmd_load_ip_fw){0x240000,0xf4,0x1000,3};
  struct psp_gfx_cmd_resp before=cmd;
  expected_before=scenario==2||scenario==3?0:scenario==4?1:2;
  assert(!psp_ring_cmd_submit(&p,p.cmd_buf_mc_addr,p.fence_buf_mc_addr,p.fence_value));
  assert(calls==1 && flushes==1 && !memcmp(&cmd,&before,sizeof(cmd)));
  assert(logs==expected_before+(expected_before!=0));
  if(expected_before) {
   assert(strstr(records[0],"publish enter") && strstr(records[logs-1],"publish returned"));
   if(cmd.cmd_id==5 || cmd.cmd_id==9) {
    assert(strstr(records[1],"gpu=f41e000000 system=9e000000"));
    assert(strstr(records[1],"size=a00000 flags=2"));
   } else if(cmd.cmd_id==6) {
    assert(strstr(records[1],"type=3 gpu=f400240000 size=1000"));
   }
  }
 }
 logs=calls=flushes=0; old_wptr=1040;
 assert(psp_ring_cmd_submit(&p,p.cmd_buf_mc_addr,p.fence_buf_mc_addr,1)==-EINVAL);
 assert(!calls&&!flushes&&!logs);
 puts("PASS: actual TMR preparation and ring submission, GPU/system address distinction, publish ordering/wrap, intact payload and callback, bounded logs, original bounds errors");
}
'''
with tempfile.TemporaryDirectory(prefix='psp-submit-trace-') as tmp:
    p=Path(tmp)
    (p/'amdgpu.h').write_text('')
    (p/'linuxkpi').mkdir()
    (p/'linuxkpi/log.h').write_text('')
    trace=ROOT/'kernel/linuxkpi/src/psp_submit_trace.c'
    header=ROOT/'kernel/linuxkpi/include/linuxkpi/psp_ring_trace.h'
    source=SHIM+trace.read_text()+header.read_text()
    source+=function('psp_prep_tmr_cmd_buf')+function('psp_ring_cmd_submit')+MAIN
    (p/'test.c').write_text(source)
    subprocess.run(['cc','-std=gnu11','-Wall','-Wextra','-Werror','-g','-fsanitize=address,undefined',
                    '-DAVORYOS_AMDGPU_MEMORY_POLICY','-I'+tmp,str(p/'test.c'),'-o',str(p/'test')],check=True)
    subprocess.run([str(p/'test')],check=True,timeout=10)
    subprocess.run(['cc','-nostdinc','-c',str(trace),'-o',str(p/'disabled.o')],check=True)
