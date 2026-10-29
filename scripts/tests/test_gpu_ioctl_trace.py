#!/usr/bin/env python3
"""Check actual native file bridge CS tracing through AMDGPU's ioctl wrapper."""
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[2]
src=(root/'kernel/linuxkpi/src/file.c').read_text()
src=src[src.index('int linuxkpi_file_ioctl('):src.index('\nstatic unsigned int kpi_poll_mask_to_native')]
code=r'''
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <linuxkpi/gpu_diag.h>
#define __user
#define DRM_IOCTL_AMDGPU_CS 0xc0186444u
#define DRM_IOCTL_AMDGPU_GEM_CREATE 0xc0206440u
#define DRM_IOCTL_PRIME_HANDLE_TO_FD 0xc00c642du
struct drm_driver{const char *name;};struct drm_device{struct drm_driver *driver;};
struct drm_minor{struct drm_device *dev;};struct drm_file{struct drm_minor *minor;};
struct file;struct file_operations{long(*unlocked_ioctl)(struct file*,unsigned int,unsigned long);};
struct file{struct file_operations *f_op;void *private_data;};
union drm_amdgpu_cs{struct{uint32_t ctx_id,bo_list_handle,num_chunks,flags;uint64_t chunks;}in;struct{uint64_t handle;}out;};
static struct file *kpi_node_file(void *p){return p;}
static unsigned int level=1,count;
static unsigned int events[4];static uint64_t last_ctx,last_seq;
static unsigned int error_stage,error_count;
void gpu_diag_prime_error(unsigned int stage,const void *o,uint64_t d,uint64_t n,long r){
 (void)o;(void)n;assert(r<0 && (d==DRM_IOCTL_AMDGPU_GEM_CREATE || d==DRM_IOCTL_PRIME_HANDLE_TO_FD));
 error_stage=stage;error_count++;
}
static unsigned int gpu_diag_level(void){return level;}
void gpu_diag_record(unsigned int k,const void *o,uint64_t a,uint64_t b,uint64_t c,uint64_t d,long r){
 (void)o;(void)c;(void)d;(void)r;assert(count<4);events[count++]=k;
 if(k==GPU_DIAG_CS_OUT){last_ctx=a;last_seq=b;}
}
static unsigned long long linuxkpi_monotonic_ms(void){return 1;}
static int copy_from_user(void *to,const void *from,size_t n){if((uintptr_t)from<4096)return 1;memcpy(to,from,n);return 0;}
static long drm_ioctl(struct file *f,unsigned int r,unsigned long a){(void)f;(void)r;if(a<4096)return -EFAULT;((union drm_amdgpu_cs*)a)->out.handle=99;return 0;}
static long amdgpu_drm_ioctl(struct file *f,unsigned int r,unsigned long a){return drm_ioctl(f,r,a);}
static long unrelated_ioctl(struct file *f,unsigned int r,unsigned long a){(void)f;(void)r;(void)a;return -ENOTTY;}
'''.replace('static unsigned int gpu_diag_level(void)','unsigned int gpu_diag_level(void)')+src+r'''
int main(void){
 struct drm_driver driver={"amdgpu"};struct drm_device dev={&driver};struct drm_minor minor={&dev};struct drm_file df={&minor};
 struct file_operations ops={amdgpu_drm_ioctl};struct file f={&ops,&df};
 union drm_amdgpu_cs u={.in={.ctx_id=7,.bo_list_handle=3,.num_chunks=2,.flags=5}};
 assert(!linuxkpi_file_ioctl(&f,DRM_IOCTL_AMDGPU_CS,(uintptr_t)&u));
 assert(count==3 && events[0]==GPU_DIAG_CS_IN && events[1]==GPU_DIAG_IOCTL && events[2]==GPU_DIAG_CS_OUT);
 assert(last_ctx==7 && last_seq==99);
 count=0;assert(linuxkpi_file_ioctl(&f,DRM_IOCTL_AMDGPU_CS,1)==-EFAULT);assert(count==1 && events[0]==GPU_DIAG_IOCTL);
 count=0;assert(linuxkpi_file_ioctl(&f,DRM_IOCTL_AMDGPU_GEM_CREATE,1)==-EFAULT);
 assert(count==1 && error_count==1 && error_stage==GPU_PRIME_SOURCE_ALLOC);
 count=0;assert(linuxkpi_file_ioctl(&f,DRM_IOCTL_PRIME_HANDLE_TO_FD,1)==-EFAULT);
 assert(count==1 && error_count==2 && error_stage==GPU_PRIME_SOURCE_EXPORT);
 count=0;ops.unlocked_ioctl=unrelated_ioctl;f.private_data=(void*)1;
 assert(linuxkpi_file_ioctl(&f,DRM_IOCTL_AMDGPU_CS,1)==-ENOTTY);assert(count==1);
 count=0;level=0;ops.unlocked_ioctl=amdgpu_drm_ioctl;f.private_data=&df;
 assert(!linuxkpi_file_ioctl(&f,DRM_IOCTL_AMDGPU_CS,(uintptr_t)&u));assert(!count);
 puts("PASS: actual ioctl bridge traces AMD wrapper CS context/result, failed copies, unrelated private_data and disabled tracing");
}
'''
with tempfile.TemporaryDirectory(prefix='gpu-ioctl-') as temp:
 p=Path(temp);(p/'check.c').write_text(code)
 subprocess.run(['cc','-O2','-Wall','-Wextra','-Werror','-Wno-address','-I'+str(root/'kernel/linuxkpi/include'),str(p/'check.c'),'-o',str(p/'check')],check=True)
 subprocess.run([str(p/'check')],check=True)
