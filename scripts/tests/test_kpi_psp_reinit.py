#!/usr/bin/env python3
"""Scoped PSP reinit with actual imported stop/create/wait on simulated MMIO."""
from pathlib import Path
import ast,re,subprocess,tempfile
ROOT=Path(__file__).resolve().parents[2]
prior=ast.parse((ROOT/'scripts/tests/test_kpi_psp_ring.py').read_text())
shim=next(ast.literal_eval(n.value) for n in prior.body if isinstance(n,ast.Assign) and any(isinstance(t,ast.Name) and t.id=='shim' for t in n.targets))
shim=shim.replace('struct amdgpu_device {unsigned usec_timeout;bool no_hw_access,vf;};', '''
struct pci_dev {unsigned vendor,device;};
struct amdgpu_device {unsigned usec_timeout;bool no_hw_access,vf;struct pci_dev *pdev;unsigned flags;bool passthrough;unsigned ip_versions[1][1];};
#define AMD_IS_APU 1
#define MP0_HWIP 0
#define IP_VERSION(a,b,c) (((a)<<16)|((b)<<8)|(c))
#define amdgpu_passthrough(a) ((a)->passthrough)
#define module_param(name,type,perm)
#define MODULE_PARM_DESC(name,desc)
''')
shim=shim.replace('struct psp_funcs {int (*ring_create)(struct psp_context*,enum psp_ring_type);};', 'struct psp_funcs {int (*ring_create)(struct psp_context*,enum psp_ring_type);int (*ring_stop)(struct psp_context*,enum psp_ring_type);};')
shim=shim.replace('static uint32_t response,observed;', 'static uint32_t response,observed,stop_response,create_response;static unsigned stop_logs;')
shim=shim.replace('registers[reg]=val;command_reads=0;', '''registers[reg]=val;command_reads=0;
 if(reg==64 && val==0x30000){response=stop_response;if((response&0x8000ffff)==0x80000000)create_response=0x80020000;}
 if(reg==64 && val==0x20000)response=create_response;
''')
shim=shim.replace('if(strstr(format,"PSP wait failed")){','if(strstr(format,"PSP reinit stop")){stop_logs++;}else if(strstr(format,"PSP wait failed")){')
def function(path,name):
 s=(ROOT/path).read_text();m=re.search(r'^(?:static )?int '+name+r'\([^;]*?\)\s*\{',s,re.M);assert m,name
 n=s.index('{',m.start())+1;depth=1
 while depth:depth+=(s[n]=='{')-(s[n]=='}');n+=1
 return s[m.start():n]
code=shim+function('kernel/linux/drivers/gpu/drm/amd/amdgpu/amdgpu_psp.c','psp_wait_for').replace('int psp_wait_for(','int linuxkpi_original_psp_wait_for(',1)
code+=function('kernel/linuxkpi/src/amdgpu_trace.c','psp_wait_for')
for name in ('psp_v13_0_ring_stop','psp_v13_0_ring_create'):code+=function('kernel/linux/drivers/gpu/drm/amd/amdgpu/psp_v13_0.c',name)
code+=function('kernel/linuxkpi/src/amdgpu_trace.c','linuxkpi_psp_ring_create')
code+=re.sub(r'^#include.*$','',(ROOT/'kernel/linuxkpi/src/psp_reinit.c').read_text(),flags=re.M)
code+=r'''
static void reset(void){
 memset(registers,0,sizeof(registers));reads=writes=poll_delay_us=handshake_ms=errors=enters=exits=wait_logs=command_reads=stop_logs=0;
 ready_delay=1;response=0x80000000;observed=0;original_result=0;
 stop_response=0x80030000;create_response=0x80020115;registers[64]=0x80020000;
}
int main(void){
 const struct psp_funcs funcs={psp_v13_0_ring_create,psp_v13_0_ring_stop};
 struct pci_dev pci={0x1002,0x164e};
 struct amdgpu_device adev={.usec_timeout=4,.pdev=&pci,.flags=AMD_IS_APU,.passthrough=true,.ip_versions={{IP_VERSION(13,0,5)}}};
 struct psp_context p={.adev=&adev,.km_ring={0xf400010000ULL,4096},.funcs=&funcs};
 assert(kpi_amdgpu_psp_reinit);
 kpi_amdgpu_psp_reinit=false;
 reset();assert(linuxkpi_psp_reinit_ring_create(&p,PSP_RING_TYPE__KM)==-ETIME);
 assert(writes==4&&!stop_logs&&observed==0x80020115);
 kpi_amdgpu_psp_reinit=true;
 reset();assert(!linuxkpi_psp_reinit_ring_create(&p,PSP_RING_TYPE__KM));
 assert(writes==5&&write_regs[0]==64&&write_values[0]==0x30000&&write_values[4]==0x20000);
 assert(stop_logs==2&&enters==1&&exits==1&&handshake_ms==40);
 reset();stop_response=0x80030001;
 assert(linuxkpi_psp_reinit_ring_create(&p,PSP_RING_TYPE__KM)==-ETIME);
 assert(writes==1&&!enters&&!exits&&stop_logs==2&&observed==0x80030001);
 reset();stop_response=0;
 assert(linuxkpi_psp_reinit_ring_create(&p,PSP_RING_TYPE__KM)==-ETIME);
 assert(writes==1&&!enters&&!exits&&stop_logs==2);
 reset();registers[64]=0;
 assert(linuxkpi_psp_reinit_ring_create(&p,PSP_RING_TYPE__KM)==-ETIME);
 assert(!writes&&!stop_logs&&!enters);
 reset();struct psp_funcs missing={psp_v13_0_ring_create,NULL};p.funcs=&missing;
 assert(linuxkpi_psp_reinit_ring_create(&p,PSP_RING_TYPE__KM)==-EOPNOTSUPP&&!writes);p.funcs=&funcs;
 for(unsigned n=0;n<4;n++){
  reset();create_response=0x80020000;
  switch(n){case 0:pci.device=0x9999;break;case 1:adev.passthrough=false;break;case 2:adev.flags=0;break;case 3:adev.ip_versions[0][0]=IP_VERSION(13,0,4);break;}
  assert(!linuxkpi_psp_reinit_ring_create(&p,PSP_RING_TYPE__KM)&&writes==4&&!stop_logs);
  pci.device=0x164e;adev.passthrough=true;adev.flags=AMD_IS_APU;adev.ip_versions[0][0]=IP_VERSION(13,0,5);
 }
 puts("PASS: original PF rejection retained when disabled; stop/checked-ack/create ordering, stop timeout/status rejection prevents create, missing callback and device/version scoping (simulated responses, no status decoding)");
}
'''
with tempfile.TemporaryDirectory(prefix='psp-reinit-') as tmp:
 p=Path(tmp);(p/'test.c').write_text(code)
 subprocess.run(['cc','-std=gnu11','-O1','-g','-DAVORYOS_AMDGPU_MEMORY_POLICY','-fsanitize=address,undefined',str(p/'test.c'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True,timeout=10)
 subprocess.run(['cc','-nostdinc','-c',str(ROOT/'kernel/linuxkpi/src/psp_reinit.c'),'-o',str(p/'disabled.o')],check=True)
