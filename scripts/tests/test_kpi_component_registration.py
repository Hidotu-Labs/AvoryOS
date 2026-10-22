#!/usr/bin/env python3
"""Exercise real component registration and AMD audio without an HDA master."""
from pathlib import Path
import subprocess,tempfile
ROOT=Path(__file__).resolve().parents[2]
def extract(source,name):
 start=source.index(name);brace=source.index('{',start);end=brace+1;depth=1
 while depth:
  depth+=(source[end]=='{')-(source[end]=='}');end+=1
 return source[start:end]+'\n'
component=(ROOT/'kernel/linuxkpi/src/component.c').read_text()
component='\n'.join(x for x in component.splitlines() if not x.startswith('#include'))
amd=(ROOT/'kernel/linux/drivers/gpu/drm/amd/display/amdgpu_dm/amdgpu_dm.c').read_text()
audio=extract(amd,'static int amdgpu_dm_audio_init(')+extract(amd,'static void amdgpu_dm_audio_fini(')
pre=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#define GFP_KERNEL 0
#define DEFINE_MUTEX(n) pthread_mutex_t n=PTHREAD_MUTEX_INITIALIZER
#define mutex_lock(n) pthread_mutex_lock(n)
#define mutex_unlock(n) pthread_mutex_unlock(n)
struct device {int id;};
struct component_ops {int (*bind)(struct device*,struct device*,void*);void (*unbind)(struct device*,struct device*,void*);};
static atomic_int live;
static bool fail_alloc;
static void *kzalloc(size_t n,int f){(void)f;if(fail_alloc)return NULL;void *p=calloc(1,n);assert(p);live++;return p;}
static void kfree(void *p){if(p){assert(live>0);live--;free(p);}}
static int binds;
static int bind(struct device *a,struct device *b,void *c){(void)a;(void)b;(void)c;binds++;return 0;}
static const struct component_ops amdgpu_dm_audio_component_bind_ops={.bind=bind};
struct pin {int channels,rate,bits_per_sample,status_bits,category_code;bool connected;int id,offset;};
struct hw_audio {int inst;};
struct resource_pool {int audio_count;struct hw_audio *audios[2];};
struct dc {struct resource_pool *res_pool;};
struct amdgpu_device {struct device *dev;struct {struct {bool enabled;int num_pins;struct pin pin[2];} audio;} mode_info;struct {struct dc *dc;bool audio_registered;} dm;};
static int amdgpu_audio=-1;
'''
post=r'''
static void *stress(void *arg){struct device dev={.id=(int)(long)arg};for(int i=0;i<1000;i++){assert(!component_add(&dev,&amdgpu_dm_audio_component_bind_ops));component_del(&dev,&amdgpu_dm_audio_component_bind_ops);}return NULL;}
int main(void){
 struct device dev={0};struct hw_audio hw[2]={{3},{7}};struct resource_pool pool={2,{&hw[0],&hw[1]}};struct dc dc={&pool};
 struct amdgpu_device adev={.dev=&dev,.dm.dc=&dc};
 assert(!amdgpu_dm_audio_init(&adev) && "AMD must register an unbound audio component");
 assert(adev.dm.audio_registered && adev.mode_info.audio.enabled && adev.mode_info.audio.num_pins==2);
 assert(adev.mode_info.audio.pin[0].id==3 && adev.mode_info.audio.pin[1].id==7 && !binds && live==1);
 assert(component_add(&dev,&amdgpu_dm_audio_component_bind_ops)==-EBUSY && live==1);
 amdgpu_dm_audio_fini(&adev);assert(!adev.dm.audio_registered && !adev.mode_info.audio.enabled && !live);
 amdgpu_dm_audio_fini(&adev);component_del(&dev,&amdgpu_dm_audio_component_bind_ops);assert(!live);
 fail_alloc=true;assert(amdgpu_dm_audio_init(&adev)==-ENOMEM && !adev.dm.audio_registered && !live);amdgpu_dm_audio_fini(&adev);fail_alloc=false;
 assert(component_add(NULL,&amdgpu_dm_audio_component_bind_ops)==-EINVAL);assert(component_add(&dev,NULL)==-EINVAL);
 amdgpu_audio=0;assert(!amdgpu_dm_audio_init(&adev) && !live && !adev.mode_info.audio.enabled);
 pthread_t threads[4];for(long i=0;i<4;i++)assert(!pthread_create(&threads[i],NULL,stress,(void*)i));for(int i=0;i<4;i++)pthread_join(threads[i],NULL);
 assert(!components && !live && !binds);
 puts("PASS: actual AMD audio registration without a master, duplicate/OOM/disable/removal, 4000 concurrent registrations and zero binds/leaks");
}
'''
with tempfile.TemporaryDirectory(prefix='kpi-component-') as tmp:
 for name,body in [('old','int component_add(struct device *d,const struct component_ops *o){(void)d;(void)o;return -EOPNOTSUPP;}\nvoid component_del(struct device *d,const struct component_ops *o){(void)d;(void)o;}\nstatic void *components;'),('fixed',component)]:
  p=Path(tmp)/name;p.with_suffix('.c').write_text(pre+body+audio+post)
  subprocess.run(['cc','-std=gnu11','-pthread','-fsanitize=address,undefined',str(p.with_suffix('.c')),'-o',str(p)],check=True)
  r=subprocess.run([str(p)],capture_output=True,text=True,timeout=20)
  if name=='old':
   assert r.returncode and 'AMD must register an unbound audio component' in r.stderr,r.stderr
   print('PASS: prior unsupported-return regression fails the actual AMD audio initializer')
  else:
   assert not r.returncode,r.stdout+r.stderr;print(r.stdout.strip())
