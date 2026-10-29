#!/usr/bin/env python3
"""Completed single timeline waits avoid allocation; real waits stay upstream."""
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
source = (ROOT/'kernel/linuxkpi/src/drm_syncobj_ready.c').read_text()
source = re.sub(r'^#include.*$', '', source, flags=re.M)
shim = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
typedef uint32_t u32;
typedef uint64_t u64;
#define DRIVER_SYNCOBJ_TIMELINE 1
#define DRM_SYNCOBJ_WAIT_FLAGS_WAIT_ALL 1
#define DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT 2
#define DRM_SYNCOBJ_WAIT_FLAGS_WAIT_AVAILABLE 4
#define DRM_SYNCOBJ_WAIT_FLAGS_WAIT_DEADLINE 8
#define u64_to_user_ptr(address) ((void*)(uintptr_t)(address))
struct drm_device {bool supported;};
struct drm_file {unsigned unused;};
struct drm_syncobj_timeline_wait {u64 handles,points;int64_t timeout_nsec;u32 count_handles,flags,first_signaled,pad;u64 deadline_nsec;};
struct drm_syncobj {unsigned references;};
struct dma_fence {unsigned references;bool signaled;};
static struct drm_syncobj object;
static struct dma_fence timeline;
static bool absent,pruned;
static unsigned slow_calls,copies,copy_fault;
static int chain_error;
static bool drm_core_check_feature(struct drm_device *device,unsigned feature){assert(feature==1);return device->supported;}
static unsigned copy_from_user(void *destination,const void *source,size_t size){
 if(++copies==copy_fault||!source)return 1;
 memcpy(destination,source,size);return 0;
}
static struct drm_syncobj *drm_syncobj_find(struct drm_file *file,u32 handle){
 assert(file);if(handle!=7)return NULL;object.references++;return &object;
}
static void drm_syncobj_put(struct drm_syncobj *syncobj){assert(syncobj==&object&&object.references>1);object.references--;}
static struct dma_fence *drm_syncobj_fence_get(struct drm_syncobj *syncobj){
 assert(syncobj==&object&&object.references>1);
 if(absent)return NULL;timeline.references++;return &timeline;
}
static void dma_fence_put(struct dma_fence *fence){if(fence){assert(fence==&timeline&&timeline.references>1);timeline.references--;}}
static int dma_fence_chain_find_seqno(struct dma_fence **fence,u64 point){
 assert(*fence==&timeline&&point==123);
 if(chain_error)return chain_error;
 if(pruned){dma_fence_put(*fence);*fence=NULL;}return 0;
}
static bool dma_fence_is_signaled(struct dma_fence *fence){assert(fence==&timeline&&fence->references>1);return fence->signaled;}
int linuxkpi_original_drm_syncobj_timeline_wait_ioctl(struct drm_device *device,void *data,struct drm_file *file){
 (void)device;(void)data;(void)file;assert(object.references==1&&timeline.references==1);slow_calls++;return -123;
}
'''
test = r'''
int main(void){
 struct drm_device device={true};struct drm_file file={0};u32 handle=7;u64 point=123;
 struct drm_syncobj_timeline_wait args;
 for(unsigned scenario=0;scenario<11;scenario++)for(unsigned flags=0;flags<16;flags++){
  args=(struct drm_syncobj_timeline_wait){.handles=(uintptr_t)&handle,.points=(uintptr_t)&point,
   .timeout_nsec=0,.count_handles=1,.flags=flags,.first_signaled=99,.deadline_nsec=1234};
  handle=7;absent=pruned=false;copies=copy_fault=chain_error=slow_calls=0;
  object.references=timeline.references=1;timeline.signaled=true;device.supported=true;
  switch(scenario){
   case 1:timeline.signaled=false;break;
   case 2:absent=true;break;
   case 3:pruned=true;break;
   case 4:chain_error=-22;break;
   case 5:copy_fault=1;break;
   case 6:copy_fault=2;break;
   case 7:handle=8;break;
   case 8:device.supported=false;break;
   case 9:args.count_handles=2;break;
   case 10:args.flags|=0x100;break;
  }
  bool ready=scenario==0||scenario==3||
             (scenario==1&&(flags&DRM_SYNCOBJ_WAIT_FLAGS_WAIT_AVAILABLE));
  int result=drm_syncobj_timeline_wait_ioctl(&device,&args,&file);
  assert(result==(ready?0:-123));assert(slow_calls==(ready?0u:1u));
  assert(args.first_signaled==(ready?0u:99u));
  assert(object.references==1&&timeline.references==1);
 }
 args.count_handles=0;slow_calls=0;
 assert(drm_syncobj_timeline_wait_ioctl(&device,&args,&file)==-123&&slow_calls==1);
 return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='avory-syncobj-ready-') as temporary:
    directory = Path(temporary)
    (directory/'test.c').write_text(shim+source+test)
    subprocess.run(['cc','-O2','-Wall','-Wextra','-Werror','-Wno-misleading-indentation',
                    str(directory/'test.c'),'-o',str(directory/'test')],check=True)
    subprocess.run([str(directory/'test')],check=True,timeout=10)
print('PASS: 176 ready/pending/available/pruned/missing/faulted timeline cases, exact flags, balanced references and upstream fallback')
