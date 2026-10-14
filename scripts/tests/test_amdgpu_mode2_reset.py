#!/usr/bin/env python3
"""CPU-only MODE2 policy and imported reset-dispatch regression; no hardware."""
from pathlib import Path
import re
import subprocess
import tempfile
ROOT=Path(__file__).resolve().parents[2]
AMD=ROOT/'kernel/linux/drivers/gpu/drm/amd/amdgpu'

def function(name):
 s=(AMD/'nv.c').read_text()
 m=re.search(r'^static (?:int|bool|enum amd_reset_method)\s*\n?'+name+r'\([^;]*?\)\s*\{',s,re.M)
 assert m,name
 n=s.index('{',m.start())+1;depth=1
 while depth:
  depth+=(s[n]=='{')-(s[n]=='}');n+=1
 return s[m.start():n]

shim=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
typedef uint32_t u32;
#define AMD_IS_APU 1
#define MP0_HWIP 0
#define MP1_HWIP 1
#define IP_VERSION(a,b,c) (((a)<<16)|((b)<<8)|(c))
enum amd_reset_method {AMD_RESET_METHOD_NONE=-1,AMD_RESET_METHOD_MODE1=1,AMD_RESET_METHOD_MODE2=2,AMD_RESET_METHOD_BACO=3,AMD_RESET_METHOD_PCI=4};
static int amdgpu_reset_method=AMD_RESET_METHOD_MODE1;
struct pci_dev {unsigned vendor,device;};
struct amdgpu_device;
struct amdgpu_asic_funcs {bool (*need_reset_on_init)(struct amdgpu_device*);int (*reset)(struct amdgpu_device*);};
struct nbio_funcs {u32 (*get_memsize)(struct amdgpu_device*);};
struct amdgpu_device {struct pci_dev *pdev;unsigned flags;bool passthrough,vf;
 struct {struct {unsigned num_physical_nodes;}xgmi;}gmc;
 unsigned ip_versions[2][1];const struct amdgpu_asic_funcs *asic_funcs;
 struct {const struct nbio_funcs *funcs;}nbio;unsigned usec_timeout;void *dev;};
#define amdgpu_passthrough(a) ((a)->passthrough)
#define amdgpu_sriov_vf(a) ((a)->vf)
#define module_param(name,type,perm) _Static_assert(sizeof(name)==sizeof(bool),"bool parameter")
#define MODULE_PARM_DESC(name,desc)
#define dev_warn(...) ((void)0)
#define dev_info(...) ((void)0)
#define dev_err(...) ((void)0)
static unsigned need_calls,reset_calls,mode2_calls,other_calls,phase,logs;
static int result;
static bool ready=true;
static unsigned mem_reads;
static bool original_need(struct amdgpu_device *a){assert(a);need_calls++;return false;}
static bool amdgpu_dpm_is_baco_supported(struct amdgpu_device *a){(void)a;return false;}
static void amdgpu_atombios_scratch_regs_engine_hung(struct amdgpu_device *a,bool b){assert(a);if(b){assert(phase==0);phase=1;}else{assert(phase==5);phase=6;}}
static void pci_clear_master(struct pci_dev *p){assert(p&&phase==1);phase=2;}
static bool amdgpu_device_cache_pci_state(struct pci_dev *p){assert(p&&phase==2);phase=3;return true;}
static int amdgpu_dpm_mode2_reset(struct amdgpu_device *a){assert(a&&phase==3);phase=4;mode2_calls++;return result;}
static bool amdgpu_device_load_pci_state(struct pci_dev *p){assert(p&&phase==4);phase=5;return true;}
static u32 memsize(struct amdgpu_device *a){assert(a&&(phase==5||phase==6));mem_reads++;return ready?512:0xffffffff;}
static void udelay(unsigned n){assert(n==1);}
static int amdgpu_device_pci_reset(struct amdgpu_device *a){(void)a;other_calls++;return -95;}
static int amdgpu_dpm_baco_reset(struct amdgpu_device *a){(void)a;other_calls++;return -95;}
static int amdgpu_device_mode1_reset(struct amdgpu_device *a){(void)a;other_calls++;return -95;}
static void klogf(const char *fmt,...){assert(fmt);logs++;}
'''
main=r'''
static int counted_reset(struct amdgpu_device *a){reset_calls++;return nv_asic_reset(a);}
static int probe_reset(struct amdgpu_device *adev){
 if(!amdgpu_sriov_vf(adev)&&amdgpu_asic_need_reset_on_init(adev)){
 int tmp=amdgpu_reset_method;
 amdgpu_reset_method=AMD_RESET_METHOD_NONE;
 int ret=amdgpu_asic_reset(adev);
 amdgpu_reset_method=tmp;
 return ret;
 }
 return 0;
}
int main(void){
 const struct amdgpu_asic_funcs funcs={original_need,counted_reset};
 const struct nbio_funcs nf={memsize};struct pci_dev pci={0x1002,0x164e};
 struct amdgpu_device baseline={.pdev=&pci,.flags=AMD_IS_APU,.passthrough=true,
 .ip_versions={{IP_VERSION(13,0,5)},{IP_VERSION(13,0,5)}},.asic_funcs=&funcs,.nbio={&nf},.usec_timeout=4};
 struct amdgpu_device a=baseline;
 assert(kpi_amdgpu_mode2_reset);
 kpi_amdgpu_mode2_reset=false;
 assert(!probe_reset(&a)&&need_calls==1&&!reset_calls&&!logs);
 kpi_amdgpu_mode2_reset=true;
 for(unsigned i=0;i<8;i++){
 a=baseline;struct pci_dev p=pci;a.pdev=&p;
 switch(i){case 0:p.vendor=0xffff;break;case 1:p.device=0xffff;break;
 case 2:a.flags=0;break;case 3:a.passthrough=false;break;
 case 4:a.vf=true;break;case 5:a.gmc.xgmi.num_physical_nodes=1;break;
 case 6:a.ip_versions[MP0_HWIP][0]=IP_VERSION(13,0,4);break;
 case 7:a.ip_versions[MP1_HWIP][0]=IP_VERSION(13,0,4);break;}
 unsigned n=need_calls;assert(!probe_reset(&a));assert(need_calls==n+(i!=4)&&!reset_calls&&!logs);
 }
 a=baseline;
 for(unsigned fail=0;fail<2;fail++){
 phase=0;result=fail?-5:0;unsigned n=need_calls;
 assert(probe_reset(&a)==result);
 assert(phase==6&&need_calls==n&&reset_calls==fail+1&&mode2_calls==fail+1&&!other_calls);
 assert(amdgpu_reset_method==AMD_RESET_METHOD_MODE1);
 }
 assert(logs==6);
 phase=0;result=0;ready=false;mem_reads=0;
 assert(probe_reset(&a)==-ETIME);
 assert(mem_reads==a.usec_timeout+1&&mode2_calls==3&&reset_calls==3&&!other_calls&&phase==6);
 assert(amdgpu_reset_method==AMD_RESET_METHOD_MODE1);
 puts("PASS: automatic scoped recovery with explicit disable, Raphael/PF/passthrough/IP scoping, imported AUTO dispatch selects MODE2, BM/PCI/SMU sequence, one attempt, errors/readiness timeout and original parameter restored");
}
'''
with tempfile.TemporaryDirectory(prefix='amdgpu-mode2-') as tmp:
 p=Path(tmp);(p/'amdgpu.h').write_text('');(p/'linux').mkdir();(p/'linuxkpi').mkdir()
 (p/'linux/moduleparam.h').write_text('');(p/'linuxkpi/log.h').write_text('')
 (p/'linux/errno.h').write_text('#define ETIME 62\n')
 owned=ROOT/'kernel/linuxkpi/src/amdgpu_init_reset.c'
 source=shim+function('nv_asic_mode2_reset')+function('nv_asic_reset_method')+function('nv_asic_reset')
 source+=owned.read_text()+(ROOT/'kernel/linuxkpi/include/linuxkpi/amdgpu_init_reset.h').read_text()+main
 (p/'test.c').write_text(source)
 subprocess.run(['cc','-std=gnu11','-Wall','-Wextra','-Werror','-g','-fsanitize=address,undefined','-DAVORYOS_AMDGPU_MEMORY_POLICY','-I'+tmp,str(p/'test.c'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True,timeout=10)
 subprocess.run(['cc','-nostdinc','-c',str(owned),'-o',str(p/'disabled.o')],check=True)
