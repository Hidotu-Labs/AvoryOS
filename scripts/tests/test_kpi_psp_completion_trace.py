#!/usr/bin/env python3
"""Actual imported PSP submission loop plus completion hook; no hardware I/O."""
from pathlib import Path
import re, subprocess, tempfile
ROOT=Path(__file__).resolve().parents[2]
s=(ROOT/'kernel/linux/drivers/gpu/drm/amd/amdgpu/amdgpu_psp.c').read_text()
m=re.search(r'^static int\s+psp_cmd_submit_buf\([^;]*?\)\s*\{',s,re.M);assert m
n=s.index('{',m.start())+1;depth=1
while depth:
 depth+=(s[n]=='{')-(s[n]=='}');n+=1
submit=s[m.start():n]
shim=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#define READ_ONCE(x) modeled_read_once(&(x))
#define atomic_read(p) (*(p))
#define atomic_inc_return(p) (++*(p))
#define atomic_dec(p) (--*(p))
#define EINVAL 22
#define TEE_ERROR_NOT_SUPPORTED 0xffff000a
#define PSP_ERR_UNKNOWN_COMMAND 0x100
#define dev_warn(...) (warnings++)
#define amdgpu_sriov_vf(a) ((a)->vf)
struct amdgpu_ring {int dummy;};
struct amdgpu_device;
struct psp_gfx_resp {uint32_t status,fw_addr_lo,fw_addr_hi;};
struct psp_gfx_cmd_resp {uint32_t cmd_id; struct psp_gfx_resp resp;};
#define PSP_CMD_BUFFER_SIZE sizeof(struct psp_gfx_cmd_resp)
struct psp_context {struct amdgpu_device *adev;void *fence_buf;
 struct psp_gfx_cmd_resp *cmd_buf_mem;int fence_value;uint64_t cmd_buf_mc_addr;};
struct amdgpu_device {bool accel_working,no_hw_access,vf;struct psp_context psp;};
struct amdgpu_firmware_info {unsigned ucode_id;uint32_t tmr_mc_addr_lo,tmr_mc_addr_hi;};
static unsigned invalidates,sleeps,logs,warnings,submits;
static int done_after,ring_result;
static bool ras;
static uint32_t response,expected_index;
static uint64_t response_gpu;
static bool completion_race;
static struct amdgpu_device *race_adev;
static uint32_t modeled_read_once(volatile uint32_t *p) {
 uint32_t old=*p;
 if(completion_race){
  completion_race=false;
  race_adev->psp.cmd_buf_mem->resp=(struct psp_gfx_resp){response,(uint32_t)response_gpu,(uint32_t)(response_gpu>>32)};
  *p=expected_index;
 }
 return old;
}
static void klogf(const char *fmt,...) {
 unsigned cmd,expected,observed,status;unsigned long long gpu;
 char line[256];va_list args;va_start(args,fmt);vsnprintf(line,sizeof(line),fmt,args);va_end(args);
 assert(sscanf(line,"[INFO] LinuxKPI: PSP fence observed cmd=%x expected=%u observed=%u status=%x fw_gpu=%llx",&cmd,&expected,&observed,&status,&gpu)==5);
 assert(cmd==6&&expected==expected_index&&observed==expected_index&&status==response&&gpu==response_gpu);logs++;
}
static void amdgpu_device_invalidate_hdp(struct amdgpu_device *a,struct amdgpu_ring *ring) {
 assert(a&&!ring);invalidates++;
 if(done_after>0&&invalidates==(unsigned)done_after){
  a->psp.cmd_buf_mem->resp=(struct psp_gfx_resp){response,(uint32_t)response_gpu,(uint32_t)(response_gpu>>32)};
  *(uint32_t*)a->psp.fence_buf=expected_index;
 }
}
static int psp_ring_cmd_submit(struct psp_context *p,uint64_t cmd,uint64_t fence,int index){
 assert(p&&cmd==0xf400202000ULL&&fence==0xf400201000ULL);submits++;expected_index=index;return ring_result;
}
static bool amdgpu_ras_intr_triggered(void){return ras;}
static void usleep_range(unsigned low,unsigned high){assert(low==60&&high==100);sleeps++;}
'''
main=r'''
int main(void){
 for(unsigned scenario=0;scenario<12;scenario++){
  struct amdgpu_device a={0};struct psp_gfx_cmd_resp shared={0},cmd={.cmd_id=6};uint32_t fence=10;
  a.psp=(struct psp_context){&a,&fence,&shared,10,0xf400202000ULL};
  struct amdgpu_firmware_info ucode={0};
  invalidates=sleeps=logs=warnings=submits=0;done_after=1;ring_result=0;ras=false;response=0;response_gpu=0xf41e001000ULL;
  completion_race=false;race_adev=&a;
  switch(scenario){
   case 1:done_after=4;break;
   case 2:response=0xffff000d;break;
   case 3:done_after=-1;break;
   case 4:ring_result=-5;break;
   case 5:done_after=-1;ras=true;break;
   case 6:a.vf=true;response=0xffff000d;break;
   case 7:a.no_hw_access=true;break;
   case 8:a.accel_working=true;break;
   case 9:a.psp.fence_value=128;break;
   case 10:a.accel_working=true;done_after=4;break;
   case 11:done_after=-1;completion_race=true;break;
  }
  int ret=psp_cmd_submit_buf(&a.psp,&ucode,&cmd,0xf400201000ULL);
  if(scenario==3)assert(ret==-22&&invalidates==20000&&!logs);
  else if(scenario==4)assert(ret==-5&&!invalidates&&!logs&&a.psp.fence_value==10);
  else if(scenario==5)assert(!ret&&invalidates==1&&!logs&&!sleeps);
  else if(scenario==6)assert(ret==-22&&logs==1&&cmd.resp.status==response);
  else if(scenario==7)assert(!ret&&!submits&&!invalidates&&!logs&&a.psp.fence_value==10);
  else if(scenario==11)assert(!ret&&invalidates==1&&!logs&&!sleeps&&fence==expected_index);
  else {
   assert(!ret&&invalidates==(unsigned)done_after&&sleeps==(unsigned)done_after-1);
   assert(logs==(scenario>=8?0:1)&&cmd.resp.status==response);
   assert(ucode.tmr_mc_addr_lo==0x1e001000&&ucode.tmr_mc_addr_hi==0xf4);
  }
  if(scenario!=7)assert(submits==1);
 }
 puts("PASS: imported PSP loop immediate/delayed completion, nonzero PF status retained, VF errors, timeout/RAS/submission failure, no-hardware path and tracing bounds; original HDP call counts unchanged");
}
'''
with tempfile.TemporaryDirectory(prefix='psp-completion-') as tmp:
 p=Path(tmp);(p/'amdgpu.h').write_text('');(p/'linuxkpi').mkdir();(p/'linuxkpi/log.h').write_text('')
 source=shim+(ROOT/'kernel/linuxkpi/src/psp_completion_trace.c').read_text()
 source+='#define amdgpu_device_invalidate_hdp(a,r) linuxkpi_psp_invalidate_hdp((a),(r))\n'+submit+main
 (p/'test.c').write_text(source)
 subprocess.run(['cc','-std=gnu11','-Wall','-Wextra','-Werror','-Wno-sign-compare','-O1','-g','-fsanitize=address,undefined','-DAVORYOS_AMDGPU_MEMORY_POLICY','-I'+tmp,str(p/'test.c'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True,timeout=10)
 subprocess.run(['cc','-nostdinc','-c',str(ROOT/'kernel/linuxkpi/src/psp_completion_trace.c'),'-o',str(p/'disabled.o')],check=True)
