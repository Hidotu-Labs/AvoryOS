#!/usr/bin/env python3
"""Actual DM init must not publish freed state or shut down destroyed DC."""
from pathlib import Path
import subprocess,tempfile
ROOT=Path(__file__).resolve().parents[2]
def extract(source,name):
 start=source.index(name);brace=source.index('{',start);end=brace+1;depth=1
 while depth:
  depth+=(source[end]=='{')-(source[end]=='}');end+=1
 return source[start:end]+'\n'
path='drivers/gpu/drm/amd/display/amdgpu_dm/amdgpu_dm.c'
fixed=extract((ROOT/'kernel/linux'/path).read_text(),'static int amdgpu_dm_mode_config_init(')
old=extract((ROOT/'build/nouveau-c0/linux-candidate'/path).read_text(),'static int amdgpu_dm_mode_config_init(')
dev=(ROOT/'kernel/linux/drivers/gpu/drm/amd/amdgpu/amdgpu_device.c').read_text()
start=dev.index('void amdgpu_device_fini_hw(');block=extract(dev[start:],'if (adev->mode_info.mode_config_initialized)')
old_block=block.replace('else if (!amdgpu_device_has_dc_support(adev) || adev->dm.dc)','else')
assert fixed!=old and block!=old_block
pre=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>
#define GFP_KERNEL 0
#define CHIP_HAWAII 1
#define AMD_PRIVATE_COLOR 1
struct drm_private_state {int token;};
struct dm_atomic_state {struct drm_private_state base;void *context;};
struct drm_private_obj {struct drm_private_state *state;};
struct drm_device {struct {void *funcs,*helper_private;int max_width,max_height,preferred_depth,prefer_shadow;bool async_page_flip;} mode_config;};
struct amdgpu_device {int asic_type;struct {bool mode_config_initialized;} mode_info;struct {void *dc;struct drm_private_obj atomic_obj;} dm;struct drm_device drm;};
static int amdgpu_dm_mode_funcs,amdgpu_dm_mode_config_helperfuncs,dm_atomic_state_funcs;
static int failure,live,published,atomic_calls,legacy_calls;static bool atomic_mode,has_dc;static struct amdgpu_device *current;
static struct drm_device *adev_to_drm(struct amdgpu_device *a){return &a->drm;}
static void *kzalloc(size_t n,int flags){(void)flags;if(failure==1)return NULL;void *p=calloc(1,n);assert(p);live++;return p;}
static void kfree(void *p){if(p){live--;free(p);}}
static void *dc_state_create_current_copy(void *dc){assert(dc);if(failure==2)return NULL;void *p=malloc(8);assert(p);live++;return p;}
static void dc_state_release(void *p){assert(p);live--;free(p);}
static void drm_atomic_private_obj_init(struct drm_device *d,struct drm_private_obj *o,struct drm_private_state *s,void *f){(void)d;(void)f;assert(!o->state);o->state=s;published++;}
static int amdgpu_display_modeset_create_props(struct amdgpu_device *a){(void)a;return failure==3?-EINVAL:0;}
static int amdgpu_dm_create_color_properties(struct amdgpu_device *a){(void)a;return failure==4;}
static int amdgpu_dm_audio_init(struct amdgpu_device *a){(void)a;return failure==5?-ENODEV:0;}
static bool drm_drv_uses_atomic_modeset(struct drm_device *d){(void)d;return atomic_mode;}
static bool amdgpu_device_has_dc_support(struct amdgpu_device *a){(void)a;return has_dc;}
static void drm_helper_force_disable_all(struct drm_device *d){(void)d;legacy_calls++;}
static void drm_atomic_helper_shutdown(struct drm_device *d){(void)d;assert((!has_dc || current->dm.dc) && "shutdown must not call destroyed Display Core");atomic_calls++;}
'''
post=r'''
int main(void){
 for(failure=1;failure<=5;failure++){
  struct amdgpu_device a={.dm.dc=(void*)1};published=0;
  int ret=amdgpu_dm_mode_config_init(&a);assert(ret<0 && !live);
  assert(!a.dm.atomic_obj.state && !published && "failed mode setup published freed state");
 }
 failure=0;struct amdgpu_device a={.dm.dc=(void*)1};published=0;assert(!amdgpu_dm_mode_config_init(&a));assert(published==1 && live==2);
 struct dm_atomic_state *s=(struct dm_atomic_state*)a.dm.atomic_obj.state;dc_state_release(s->context);kfree(s);assert(!live);
 memset(&a,0,sizeof(a));a.mode_info.mode_config_initialized=true;current=&a;
 atomic_mode=true;has_dc=true;test_shutdown_condition(&a);assert(!atomic_calls && !legacy_calls);
 a.dm.dc=(void*)1;test_shutdown_condition(&a);assert(atomic_calls==1);
 a.dm.dc=NULL;has_dc=false;test_shutdown_condition(&a);assert(atomic_calls==2);
 atomic_mode=false;test_shutdown_condition(&a);assert(legacy_calls==1);
 a.mode_info.mode_config_initialized=false;test_shutdown_condition(&a);assert(legacy_calls==1 && atomic_calls==2);
 puts("PASS: state/context/property/color/audio failures retain no published freed state; shutdown skips destroyed DC and preserves live/legacy paths");
}
'''
with tempfile.TemporaryDirectory(prefix='amdgpu-dm-unwind-') as tmp:
 for name,init,cond,message in [('old-owner',old,block,'failed mode setup published freed state'),('old-shutdown',fixed,old_block,'shutdown must not call destroyed Display Core'),('fixed',fixed,block,None)]:
  p=Path(tmp)/name;p.with_suffix('.c').write_text(pre+init+'\nstatic void test_shutdown_condition(struct amdgpu_device *adev){\n'+cond+'}\n'+post)
  subprocess.run(['cc','-std=gnu11','-fsanitize=address,undefined',str(p.with_suffix('.c')),'-o',str(p)],check=True)
  r=subprocess.run([str(p)],capture_output=True,text=True,timeout=15)
  if message:
   assert r.returncode and message in r.stderr,r.stderr;print('PASS: '+name+' negative control reproduces the error')
  else:
   assert not r.returncode,r.stdout+r.stderr;print(r.stdout.strip())
