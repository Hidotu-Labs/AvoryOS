#!/usr/bin/env python3
"""Check VCN startup wrappers delegate once and preserve all device results.

Software models only. This cannot validate VCN startup, reset or hardware state.
"""
from pathlib import Path
import subprocess
import tempfile
ROOT=Path(__file__).resolve().parents[2]
s=(ROOT/'kernel/linuxkpi/src/amdgpu_trace.c').read_text()
start=s.index('extern int linuxkpi_original_amdgpu_device_ip_set_powergating_state(')
end=s.index('/* Only gmc_v10_0.c',start)
wrapped=s[start:end]
code=r'''
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <assert.h>
#define AMD_IS_APU 1
struct pci_dev {unsigned vendor,device;};
struct amdgpu_device {
 struct pci_dev *pdev;bool no_hw_access,passthrough,vf;unsigned flags;uint64_t pg_flags;unsigned usec_timeout;
 struct {unsigned cur_state;bool indirect_sram;}vcn;
 struct {bool dpm_enabled;}pm;
};
struct amdgpu_ring {struct amdgpu_device *adev;unsigned me,ring_size,doorbell_index;bool use_doorbell;uint64_t gpu_addr,wptr;};
enum amd_ip_block_type {AMD_IP_BLOCK_TYPE_VCN,AMD_IP_BLOCK_TYPE_GFX};
enum amd_powergating_state {AMD_PG_STATE_GATE,AMD_PG_STATE_UNGATE};
#define amdgpu_passthrough(a) ((a)->passthrough)
#define amdgpu_sriov_vf(a) ((a)->vf)
static unsigned logs,power_logs,enters,exits,power_calls,ring_calls;
static void klogf(const char *format,...) {
 logs++;if(strstr(format,"VCN power"))power_logs++;
 if(strstr(format,"test enter"))enters++;
 if(strstr(format,"test exit"))exits++;
}
'''+wrapped+r'''
static struct amdgpu_device *expected;
static struct amdgpu_ring *expected_ring;
static enum amd_ip_block_type expected_type;
static enum amd_powergating_state expected_state;
static int power_result,ring_result;
int linuxkpi_original_amdgpu_device_ip_set_powergating_state(void *dev,enum amd_ip_block_type type,enum amd_powergating_state state){
 assert(dev==expected && type==expected_type && state==expected_state);power_calls++;
 if(!power_result)expected->vcn.cur_state=state;
 return power_result;
}
int linuxkpi_original_amdgpu_vcn_dec_ring_test_ring(struct amdgpu_ring *ring){
 assert(ring==expected_ring);ring_calls++;ring->wptr+=16;
 return ring_result;
}
int main(void){
 struct pci_dev pci={0x1002,0x164e};
 struct amdgpu_device base={.pdev=&pci,.flags=AMD_IS_APU,.passthrough=true,.pg_flags=0x123456789,.usec_timeout=100000,.pm={true},.vcn={0,true}},a=base;
 struct amdgpu_ring ring={.adev=&a,.me=0,.gpu_addr=0xf400200000,.ring_size=4096,.use_doorbell=true,.doorbell_index=7};
 expected=&a;expected_ring=&ring;expected_type=AMD_IP_BLOCK_TYPE_VCN;expected_state=AMD_PG_STATE_UNGATE;
 for(unsigned failure=0;failure<3;failure++){
  a=base;power_result=failure?-5:0;unsigned before=power_calls;
  assert(amdgpu_device_ip_set_powergating_state(&a,expected_type,expected_state)==power_result);
  assert(power_calls==before+1 && a.vcn.cur_state==(failure?0:1));
  ring_result=failure?-110:0;before=ring_calls;uint64_t ptr=ring.wptr;
  assert(amdgpu_vcn_dec_ring_test_ring(&ring)==ring_result && ring_calls==before+1 && ring.wptr==ptr+16);
 }
 assert(power_logs==3 && enters==3 && exits==3);
 for(unsigned i=0;i<32;i++)amdgpu_device_ip_set_powergating_state(&a,expected_type,expected_state);
 assert(power_logs==16);
 for(unsigned exclusion=0;exclusion<6;exclusion++){
  a=base;struct pci_dev p=pci;a.pdev=&p;unsigned before=logs;
  switch(exclusion){case 0:p.vendor=0xffff;break;case 1:p.device=0xffff;break;case 2:a.flags=0;break;case 3:a.passthrough=false;break;case 4:a.vf=true;break;case 5:a.no_hw_access=true;break;}
  assert(amdgpu_vcn_dec_ring_test_ring(&ring)==ring_result && logs==before);
 }
 expected_type=AMD_IP_BLOCK_TYPE_GFX;unsigned before=logs;
 assert(amdgpu_device_ip_set_powergating_state(&a,expected_type,expected_state)==power_result && logs==before);
 assert(!vcn_startup_trace_device(NULL));
 puts("PASS: VCN trace scope/cap, single delegation, original success/errors and ring writes preserved (software only)");
}
'''
with tempfile.TemporaryDirectory(prefix='avory-vcn-trace-') as td:
 p=Path(td);(p/'test.c').write_text(code)
 subprocess.run(['cc','-std=gnu11','-O1','-g','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',p/'test.c','-o',p/'test'],check=True)
 subprocess.run([p/'test'],check=True)
