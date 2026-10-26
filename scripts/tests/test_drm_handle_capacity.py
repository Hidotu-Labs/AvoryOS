#!/usr/bin/env python3
"""Exercise the real GEM registrar beyond Minecraft's earlier handle ceiling."""
from pathlib import Path
import re,subprocess,tempfile
root=Path(__file__).resolve().parents[2]
s=(root/'kernel/src/drivers/gpu/drm/drm_file.c').read_text();a=s.index('uint32_t ascentdrm_file_gem_register(');b=s.index('{',a)+1;depth=1
while depth:
 depth+=(s[b]=='{')-(s[b]=='}');b+=1
fn=s[a:b]
limit=int(re.search(r'#define DRM_MAX_HANDLES_PER_FILE\s+(\d+)',(root/'kernel/src/drivers/gpu/drm/drm.h').read_text()).group(1))
pre=r'''
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
struct drm_gem_object {unsigned refs;};
struct drm_file {struct drm_gem_object *handles[DRM_MAX_HANDLES_PER_FILE];uint32_t next_handle,handle_high_water;int lock;};
static void spinlock_acquire(int *p){assert(!*p);*p=1;}
static void spinlock_release(int *p){assert(*p);*p=0;}
static void ascentdrm_gem_get(struct drm_gem_object *p){p->refs++;}
'''
main=r'''
int main(void){
 enum{N=6000};struct drm_file *f=calloc(1,sizeof(*f));struct drm_gem_object *bo=calloc(N+2,sizeof(*bo));assert(f&&bo);f->next_handle=1;f->handle_high_water=1;
 for(unsigned i=0;i<N;i++){
  bo[i].refs=1;uint32_t h=ascentdrm_file_gem_register(f,&bo[i]);
  if(!h){fprintf(stderr,"GEM handle allocation failed at live resource %u, namespace=%u\n",i+1,DRM_MAX_HANDLES_PER_FILE);return 1;}
  assert(f->handles[h]==&bo[i]&&bo[i].refs==2);
 }
 for(unsigned i=0;i<N;i+=61){uint32_t h=ascentdrm_file_gem_register(f,&bo[i]);assert(h&&f->handles[h]==&bo[i]&&bo[i].refs==2);}
 f->handles[50]=NULL;f->next_handle=50;assert(ascentdrm_file_gem_register(f,&bo[N])==50);
 f->next_handle=0xF0B0;uint32_t reserved=ascentdrm_file_gem_register(f,&bo[N+1]);assert(reserved&&reserved!=0xF0B0);
 free(bo);free(f);puts("PASS: 6000 live GEM resources, duplicate imports, slot reuse and reserved hardware-FB handle");
}
'''
with tempfile.TemporaryDirectory(prefix='drm-handle-capacity-') as tmp:
 p=Path(tmp);code=f'#define DRM_MAX_HANDLES_PER_FILE {limit}\n'+pre+fn+main;(p/'test.c').write_text(code)
 subprocess.run(['cc','-O2','-Wall','-Wextra','-Werror',str(p/'test.c'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
 # Prove that this workload detects the previous 4096-slot implementation.
 (p/'old.c').write_text(code.replace(f'#define DRM_MAX_HANDLES_PER_FILE {limit}', '#define DRM_MAX_HANDLES_PER_FILE 4096',1))
 subprocess.run(['cc','-O2','-Wall','-Wextra','-Werror',str(p/'old.c'),'-o',str(p/'old')],check=True)
 old=subprocess.run([str(p/'old')],capture_output=True,text=True)
 assert old.returncode==1 and 'live resource 4096' in old.stderr, old.stderr
 print('PASS: previous capacity fails at live resource 4096')
