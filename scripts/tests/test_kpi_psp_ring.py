#!/usr/bin/env python3
"""Run the pinned PSP 13.x ring protocol and compatibility diagnostics on fake MMIO."""
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]


def function(path, name):
    source = (ROOT / path).read_text()
    match = re.search(r'^(?:static )?int ' + name + r'\([^;]*?\)\s*\{', source, re.M)
    assert match, name
    end = source.index('{', match.start()) + 1
    depth = 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[match.start():end]


shim = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <errno.h>
enum psp_ring_type {PSP_RING_TYPE__KM=2};
struct amdgpu_device {unsigned usec_timeout;bool no_hw_access,vf;};
struct psp_context;
struct psp_ring {uint64_t ring_mem_mc_addr;unsigned ring_size;};
struct psp_funcs {int (*ring_create)(struct psp_context*,enum psp_ring_type);};
struct psp_context {struct amdgpu_device *adev;struct psp_ring km_ring;const struct psp_funcs *funcs;};
#define MP0 0
#define regMP0_SMN_C2PMSG_64 64
#define regMP0_SMN_C2PMSG_69 69
#define regMP0_SMN_C2PMSG_70 70
#define regMP0_SMN_C2PMSG_71 71
#define regMP0_SMN_C2PMSG_101 101
#define regMP0_SMN_C2PMSG_102 102
#define regMP0_SMN_C2PMSG_103 103
#define GFX_CTRL_CMD_ID_DESTROY_GPCOM_RING 1
#define GFX_CTRL_CMD_ID_DESTROY_RINGS 0x30000
#define GFX_CTRL_CMD_ID_INIT_GPCOM_RING 2
#define SOC15_REG_OFFSET(ip,instance,reg) (reg)
#define lower_32_bits(v) ((uint32_t)(v))
#define upper_32_bits(v) ((uint32_t)((v)>>32))
static uint32_t registers[128],write_values[16];
static unsigned reads,writes,write_regs[16],poll_delay_us,handshake_ms;
static unsigned errors,enters,exits,wait_logs,command_reads,ready_delay;
static uint32_t response,observed;
static uint64_t expected_ring_addr=0xf400010000ULL;
static int original_result;
static uint32_t mmio_read(unsigned reg){
 assert(reg<128);reads++;
 if((reg==64||reg==101)&&writes&&registers[reg]!=0x80000000){
  if(++command_reads>=ready_delay)registers[reg]=response;
 }
 return registers[reg];
}
static void mmio_write(unsigned reg,uint32_t val){
 assert(reg<128&&writes<16);write_regs[writes]=reg;write_values[writes++]=val;
 registers[reg]=val;command_reads=0;
}
#define RREG32(reg) ((void)adev,mmio_read(reg))
#define WREG32_SOC15(ip,instance,reg,val) ((void)adev,mmio_write(reg,val))
#define amdgpu_sriov_vf(adev) ((adev)->vf)
#define DRM_ERROR(...) (errors++)
static void udelay(unsigned n){assert(n==1);poll_delay_us+=n;}
static void mdelay(unsigned n){assert(n==20);handshake_ms+=n;}
static void klogf(const char*format,...){
 va_list a;va_start(a,format);
 if(strstr(format,"PSP wait failed")){
  unsigned reg=va_arg(a,unsigned);assert(reg==64||reg==101);
  assert(va_arg(a,unsigned)==0x80000000);
  unsigned mask=va_arg(a,unsigned);assert(mask==0x80000000||mask==0x8000ffff);
  observed=va_arg(a,unsigned);assert(va_arg(a,int)==0);
  original_result=va_arg(a,int);wait_logs++;
 }else if(strstr(format,"ring-create enter")){
  assert(va_arg(a,unsigned)==2);assert(va_arg(a,unsigned long long)==expected_ring_addr);
  assert(va_arg(a,unsigned)==4096);assert(va_arg(a,unsigned)==4);enters++;
 }else if(strstr(format,"ring-create exit")){
  original_result=va_arg(a,int);exits++;
 }else assert(!"unexpected log");
 va_end(a);
}
'''
psp_source = 'kernel/linux/drivers/gpu/drm/amd/amdgpu/amdgpu_psp.c'
family_source = 'kernel/linux/drivers/gpu/drm/amd/amdgpu/psp_v13_0.c'
trace_source = 'kernel/linuxkpi/src/amdgpu_trace.c'
code = shim + function(psp_source, 'psp_wait_for').replace(
    'int psp_wait_for(', 'int linuxkpi_original_psp_wait_for(', 1)
code += function(trace_source, 'psp_wait_for')
code += function(family_source, 'psp_v13_0_ring_stop')
code += function(family_source, 'psp_v13_0_ring_create')
code += function(trace_source, 'linuxkpi_psp_ring_create')
code += r'''
static void reset(void){
 memset(registers,0,sizeof(registers));memset(write_regs,0,sizeof(write_regs));
 reads=writes=poll_delay_us=handshake_ms=errors=enters=exits=wait_logs=command_reads=0;
 ready_delay=1;response=0x80000000;observed=0;original_result=0;
}
int main(void){
 const struct psp_funcs funcs={.ring_create=psp_v13_0_ring_create};
 struct amdgpu_device adev={.usec_timeout=4};
 struct psp_context p={.adev=&adev,.km_ring={0xf400010000ULL,4096},.funcs=&funcs};
 reset();registers[64]=0x80000000;ready_delay=4;
 assert(!linuxkpi_psp_ring_create(&p,PSP_RING_TYPE__KM));
 assert(writes==4&&write_regs[0]==69&&write_values[0]==0x10000);
 assert(write_regs[1]==70&&write_values[1]==0xf4);
 assert(write_regs[2]==71&&write_values[2]==4096);
 assert(write_regs[3]==64&&write_values[3]==0x20000);
 assert(poll_delay_us==3&&handshake_ms==20&&!wait_logs&&enters==1&&exits==1);
 reset();assert(linuxkpi_psp_ring_create(&p,PSP_RING_TYPE__KM)==-ETIME);
 assert(!writes&&!handshake_ms&&poll_delay_us==4&&reads==5&&errors==1);
 assert(wait_logs==1&&original_result==-ETIME&&observed==0);
 reset();registers[64]=0x80000000;response=0x80000001;
 assert(linuxkpi_psp_ring_create(&p,PSP_RING_TYPE__KM)==-ETIME);
 assert(writes==4&&handshake_ms==20&&wait_logs==1&&observed==0x80000001);
 assert(original_result==-ETIME&&enters==1&&exits==1);
 /* User's actual response is retained as an opaque failed handshake.
  * This does not decode status 0x0115 or simulate real firmware execution. */
 reset();registers[64]=0x80000000;response=0x80020115;
 p.km_ring.ring_mem_mc_addr=0xf400210000ULL;
 expected_ring_addr=p.km_ring.ring_mem_mc_addr;
 assert(linuxkpi_psp_ring_create(&p,PSP_RING_TYPE__KM)==-ETIME);
 assert(writes==4&&write_values[0]==0x210000&&write_values[1]==0xf4);
 assert(wait_logs==1&&observed==0x80020115&&original_result==-ETIME);
 assert(enters==1&&exits==1);
 p.km_ring.ring_mem_mc_addr=0xf400010000ULL;
 expected_ring_addr=p.km_ring.ring_mem_mc_addr;
 reset();registers[64]=0x80000000;response=0x20000;
 assert(linuxkpi_psp_ring_create(&p,PSP_RING_TYPE__KM)==-ETIME);
 assert(writes==4&&wait_logs==1&&observed==0x20000&&reads==6);
 reset();adev.vf=true;registers[101]=0x80000000;
 assert(!linuxkpi_psp_ring_create(&p,PSP_RING_TYPE__KM));
 assert(writes==4&&write_regs[0]==101&&write_values[0]==1);
 assert(write_regs[1]==102&&write_values[1]==0x10000);
 assert(write_regs[2]==103&&write_values[2]==0xf4);
 assert(write_regs[3]==101&&write_values[3]==2&&handshake_ms==40&&!wait_logs);
 reset();adev.no_hw_access=true;
 assert(!psp_wait_for(&p,64,0x80000000,0x8000ffff,false)&&!reads&&!wait_logs);
 puts("PASS: actual PSP 13.x ready/command handshake, 40-bit ring address and write order, delayed response, missing SOS, rejected/stuck commands and SR-IOV stop/init; diagnostics preserve results and add no MMIO writes (ASan/UBSan)");
}
'''
with tempfile.TemporaryDirectory(prefix='kpi-psp-ring-') as temporary:
    directory = Path(temporary)
    cfile = directory / 'psp.c'
    cfile.write_text(code)
    executable = directory / 'psp'
    subprocess.run(['cc', '-std=gnu11', '-O1', '-g', '-fsanitize=address,undefined',
                    str(cfile), '-o', str(executable)], check=True)
    subprocess.run([str(executable)], check=True, timeout=10)
