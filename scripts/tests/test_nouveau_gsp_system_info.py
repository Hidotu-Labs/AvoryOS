#!/usr/bin/env python3
"""Exercise the actual GSP system-info producer with passthrough on/off."""
from pathlib import Path
import subprocess,tempfile
ROOT=Path(__file__).resolve().parents[2]
source=(ROOT/'kernel/linux/drivers/gpu/drm/nouveau/nvkm/subdev/gsp/r535.c').read_text()
start=source.index('static int\nr535_gsp_rpc_set_system_info(');end=source.index('\nstatic int\n',start+1)
function=source[start:end]
shim=r'''
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
typedef uint64_t u64;
struct pci_dev { unsigned id; };
struct nvkm_device;
struct functions { u64 (*resource_addr)(struct nvkm_device *,unsigned); };
struct nvkm_device { int type; struct functions *func; };
struct nvkm_subdev { struct nvkm_device *device; };
struct nvkm_gsp { struct nvkm_subdev subdev; };
struct nvkm_device_pci { struct nvkm_device device; struct pci_dev *pdev; };
typedef struct { u64 gpuPhysAddr,gpuPhysFbAddr,gpuPhysInstAddr,nvDomainBusDeviceFunc,maxUserVa; unsigned pciConfigMirrorBase,pciConfigMirrorSize,hypervisorType,bIsPassthru,acpiMethodData; } GspSystemInfo;
#define TASK_SIZE ((1UL<<47)-4096)
#define NVKM_DEVICE_TEGRA 1
#define NV_VGPU_MSG_FUNCTION_GSP_SET_SYSTEM_INFO 72
#define ENOSYS 38
#define WARN_ON(x) (x)
#define container_of(p,t,m) ((t *)((char *)(p)-offsetof(t,m)))
#define IS_ERR(p) ((intptr_t)(p)<0)
#define PTR_ERR(p) ((intptr_t)(p))
#define nvkm_info(...) ((void)0)
#define pci_dev_id(p) ((p)->id)
static bool gsp_kvm_passthrough,fail;
static GspSystemInfo payload;
static void *nvkm_gsp_rpc_get(struct nvkm_gsp *g,unsigned fn,unsigned size) { (void)g;assert(fn==72 && size==sizeof(payload));memset(&payload,0,sizeof(payload));return fail?(void *)(intptr_t)-12:&payload; }
static int nvkm_gsp_rpc_wr(struct nvkm_gsp *g,void *p,bool sync) { (void)g;assert(p==&payload && !sync);return 0; }
static void r535_gsp_acpi_info(struct nvkm_gsp *g,unsigned *p) { (void)g;(void)p; }
static u64 resource(struct nvkm_device *d,unsigned bar) { (void)d;return 0x80000000ULL+(u64)bar*0x100000000ULL; }
'''
main=r'''
int main(void) {
 struct functions funcs={resource};struct pci_dev pci={0x18};
 struct nvkm_device_pci dev={.device={0,&funcs},.pdev=&pci};
 struct nvkm_gsp gsp={.subdev={&dev.device}};
 assert(!r535_gsp_rpc_set_system_info(&gsp));
 assert(!payload.bIsPassthru && !payload.hypervisorType);
 gsp_kvm_passthrough=true;assert(!r535_gsp_rpc_set_system_info(&gsp));
 assert(payload.bIsPassthru==1 && payload.hypervisorType==3);
 assert(payload.gpuPhysAddr==resource(&dev.device,0) && payload.gpuPhysFbAddr==resource(&dev.device,1) && payload.gpuPhysInstAddr==resource(&dev.device,3));
 assert(payload.nvDomainBusDeviceFunc==0x18 && payload.maxUserVa==0x7ffffffff000ULL);
 fail=true;assert(r535_gsp_rpc_set_system_info(&gsp)==-12);
 dev.device.type=NVKM_DEVICE_TEGRA;assert(r535_gsp_rpc_set_system_info(&gsp)==-ENOSYS);
}
'''
with tempfile.TemporaryDirectory(prefix='nouveau-system-info-') as temp:
 exe=Path(temp)/'test';exe.with_suffix('.c').write_text(shim+function+main)
 subprocess.run(['cc','-std=gnu11','-Wall','-Wextra','-Werror',str(exe.with_suffix('.c')),'-o',str(exe)],check=True)
 subprocess.run([str(exe)],check=True)
print('PASS: actual GSP RPC producer: explicit KVM passthrough, default off, 64-bit BARs/user VA and error propagation')
