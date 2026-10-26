#!/usr/bin/env python3
"""A client waiting for its BO must release the global metadata gate before sleeping."""
from pathlib import Path
import subprocess
import tempfile
root=Path(__file__).resolve().parents[2]
s=(root/'kernel/src/drivers/gpu/virtio_gpu/virtio_gpu_virgl.c').read_text()
s=s[s.index('static int wait_ioctl('):]
pre=r'''
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <assert.h>
#include <stdio.h>
struct vg_wait{uint32_t handle,flags;};
struct drm_file{int unused;};struct drm_gem_object{int refs;};
static struct drm_gem_object bo={1};static unsigned ioctl_owner=0,waits;static int result;
static bool user(uint64_t p,uint32_t n){return p&&n==8;}
static struct drm_gem_object *lookup(struct drm_file*f,uint32_t h){(void)f;assert(ioctl_owner==1);if(h!=7)return NULL;bo.refs++;return &bo;}
static void put(struct drm_gem_object *o){assert(o==&bo&&bo.refs==2);bo.refs--;}
int virtio_gpu_virgl_ioctl(struct drm_file *,uint32_t,uint64_t);
static int virtio_gpu_render_wait_bo(struct drm_gem_object*o,bool nowait){assert(o==&bo&&o->refs==2&&!ioctl_owner);assert(nowait==(result==-16));assert(virtio_gpu_virgl_ioctl(NULL,0xc0086443,0)==123);waits++;return result;}
static void sched_yield(void){assert(!"WAIT held the metadata gate while sleeping");}
static int ioctl_locked(struct drm_file*f,uint32_t r,uint64_t a){(void)f;(void)r;(void)a;assert(ioctl_owner==1);return 123;}
'''
main=r'''
int main(void){struct drm_file f={0};struct vg_wait p={7,0};uint64_t arg=(uintptr_t)&p;
 assert(!virtio_gpu_virgl_ioctl(&f,0xc0086448,arg)&&waits==1&&bo.refs==1&&!ioctl_owner);
 result=-16;p.flags=1;assert(virtio_gpu_virgl_ioctl(&f,0xc0086448,arg)==-16&&waits==2&&bo.refs==1);
 result=-5;p.flags=0;assert(virtio_gpu_virgl_ioctl(&f,0xc0086448,arg)==-5&&bo.refs==1);
 unsigned before=waits;p.flags=2;assert(virtio_gpu_virgl_ioctl(&f,0xc0086448,arg)==-22);
 p.flags=0;p.handle=9;assert(virtio_gpu_virgl_ioctl(&f,0xc0086448,arg)==-2);
 assert(virtio_gpu_virgl_ioctl(&f,0xc0046448,arg)==-22);
 assert(virtio_gpu_virgl_ioctl(&f,0xc0086548,arg)==-22);
 assert(virtio_gpu_virgl_ioctl(&f,0xc0086448,0)==-14&&waits==before);
 ioctl_owner=0;assert(virtio_gpu_virgl_ioctl(&f,0xc0086443,arg)==123&&!ioctl_owner);
 puts("PASS: other client submits during WAIT without owning metadata gate, retains BO, validates UAPI and preserves errors");}
'''
with tempfile.TemporaryDirectory(prefix='virgl-wait-') as tmp:
 p=Path(tmp);(p/'test.c').write_text(pre+s+main)
 subprocess.run(['cc','-O2','-Wall','-Wextra','-Werror',str(p/'test.c'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
