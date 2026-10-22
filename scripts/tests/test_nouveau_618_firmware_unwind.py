#!/usr/bin/env python3
"""Run actual 6.18 GSP firmware loading and LinuxKPI allocation failure paths."""
from pathlib import Path
import re
import subprocess
import tempfile

ROOT=Path(__file__).resolve().parents[2]
TREE=ROOT/'build/nouveau-618/linux/drivers/gpu/drm/nouveau'

def function(path, signature):
    text=path.read_text();start=text.index(signature);end=text.index('{',start)+1;depth=1
    while depth:
        depth+=(text[end]=='{')-(text[end]=='}');end+=1
    return text[start:end]+'\n'

loader=(ROOT/'kernel/linuxkpi/src/firmware.c').read_text()
loader=re.sub(r'^#include.*$', '', loader, flags=re.M)
diag=(ROOT/'kernel/linuxkpi/include/linuxkpi/probe_diag.h').read_text()
functions=''.join([
    function(TREE/'nvkm/core/firmware.c','int\nnvkm_firmware_get('),
    function(TREE/'nvkm/core/firmware.c','void\nnvkm_firmware_put('),
    function(TREE/'nvkm/subdev/gsp/base.c','int\nnvkm_gsp_load_fw('),
    function(TREE/'nvkm/subdev/gsp/base.c','void\nnvkm_gsp_dtor_fws('),
    function(TREE/'nvkm/subdev/gsp/tu102.c','int\ntu102_gsp_load_rm('),
    function(TREE/'nvkm/subdev/gsp/tu102.c','int\ntu102_gsp_load('),
])
shim=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#define GFP_KERNEL 0
#define min(a,b) ((a)<(b)?(a):(b))
#define klogf(...) ((void)0)
#define nvkm_debug(...) ((void)0)
struct device { int unused; };
struct firmware { size_t size; const unsigned char *data; void *priv; };
struct nvkm_device_chip { const char *name; };
struct nvkm_device { const char *cfgopt; const struct nvkm_device_chip *chip; struct device *dev; };
struct nvkm_subdev { struct nvkm_device *device; };
struct nvkm_gsp { struct nvkm_subdev subdev; struct {
 const struct firmware *fmc,*bl,*rm;
 struct { const struct firmware *load,*unload; } booter;
} fws; };
struct nvkm_gsp_fwif { const char *ver; };
static bool nvkm_boolopt(const char *cfg,const char *key,bool def) {
 (void)key;return cfg&&strcmp(cfg,"off")==0 ? false : def;
}
static int strscpy(char*d,const char*s,size_t n){return snprintf(d,n,"%s",s);}
static const char *names[]={"gsp","bootloader","booter_load","booter_unload"};
struct blob { unsigned size,id; };
static struct blob blobs[]={{63571696,0},{36972,1},{57720,2},{41592,3}};
static int missing=-1,truncated=-1,fail_alloc;
static unsigned calls,live,opened,closed;
static void *asc_vfs_kernel_open(const char *path) {
 for(unsigned i=0;i<4;i++) {
  char expected[160];snprintf(expected,sizeof(expected),"/lib/firmware/nvidia/ad106/gsp/%s-570.144.bin",names[i]);
  if(!strcmp(path,expected)){if(missing==(int)i)return NULL;opened++;return &blobs[i];}
 }
 assert(!"unexpected firmware request");return NULL;
}
static unsigned long long asc_vfs_kernel_size64(void *p){return ((struct blob*)p)->size;}
static void asc_vfs_kernel_close(void*p){assert(p);closed++;}
static unsigned asc_vfs_kernel_read(void*p,unsigned off,unsigned count,unsigned char*buf){
 struct blob*b=p;unsigned limit=truncated==(int)b->id?b->size/2:b->size;
 assert(count<=1024*1024);if(off>=limit)return 0;
 unsigned n=min(count,limit-off);memset(buf,(unsigned char)b->id+1,n);return n;
}
static void *allocate(size_t n){if(++calls==(unsigned)fail_alloc)return NULL;void*p=malloc(n);assert(p);live++;return p;}
static void *kzalloc(size_t n,int flags){(void)flags;void*p=allocate(n);if(p)memset(p,0,n);return p;}
static void *kmalloc(size_t n,int flags){(void)flags;return allocate(n);}
static void *vmalloc(size_t n){return allocate(n);}
static void kfree(const void*p){if(p){assert(live);live--;free((void*)p);}}
static void kvfree(const void*p){kfree(p);}
'''
main=r'''
static void empty(struct nvkm_gsp*g) {
 struct linuxkpi_firmware_usage u;linuxkpi_firmware_usage(&u);
 assert(!g->fws.rm&&!g->fws.bl&&!g->fws.fmc&&!g->fws.booter.load&&!g->fws.booter.unload&&"firmware ownership not released");
 assert(!live&&!u.headers&&!u.buffers&&!u.bytes&&opened==closed);
}
static void reset(void){calls=0;missing=truncated=-1;fail_alloc=0;}
int main(void){
 struct device parent={0};struct nvkm_device_chip chip={.name="AD106"};
 struct nvkm_device device={.chip=&chip,.dev=&parent};
 struct nvkm_gsp g={.subdev.device=&device};struct nvkm_gsp_fwif fwif={.ver="570.144"};
 for(int i=0;i<4;i++){
  reset();missing=i;unsigned long n=linuxkpi_firmware_load_count();
  assert(tu102_gsp_load(&g,1,&fwif)==-ENOENT);empty(&g);
  assert(linuxkpi_firmware_load_count()-n==(unsigned)i);
  nvkm_gsp_dtor_fws(&g);empty(&g);
  reset();truncated=i;assert(tu102_gsp_load(&g,1,&fwif)==-ENOENT);empty(&g);
 }
 for(int i=1;i<=8;i++){
  reset();fail_alloc=i;assert(tu102_gsp_load(&g,1,&fwif)==-ENOENT);empty(&g);
 }
 reset();assert(!tu102_gsp_load(&g,1,&fwif));assert(live==8);
 assert(g.fws.rm->size==63571696);nvkm_gsp_dtor_fws(&g);empty(&g);
 nvkm_gsp_dtor_fws(&g);empty(&g);
 device.cfgopt="off";assert(tu102_gsp_load(&g,1,&fwif)==-EINVAL);empty(&g);
 puts("PASS: actual 6.18 GSP loader, four missing/short-read stages, eight allocator failures, 63.6MB load, recovery and idempotent release");
}
'''
negative=functions.replace('if (ret)\n\t\tnvkm_gsp_dtor_fws(gsp);','if (ret)\n\t\t(void)gsp;')
assert negative!=functions
with tempfile.TemporaryDirectory(prefix='nouveau-618-fw-') as tmp:
    for name,body in [('broken',negative),('fixed',functions)]:
        path=Path(tmp)/f'{name}.c';exe=Path(tmp)/name
        path.write_text(shim+diag+loader+body+main)
        subprocess.run(['cc','-std=gnu11','-O1','-g','-Wall','-Wextra','-Werror',
                        '-Wno-unused-parameter','-fsanitize=address,undefined',str(path),'-o',str(exe)],check=True)
        run=subprocess.run([str(exe)],capture_output=True,text=True,timeout=60)
        if name=='broken':
            assert run.returncode!=0 and 'firmware ownership not released' in run.stderr,run.stderr
            print('PASS: removing upstream firmware unwind fails the same ownership check')
        else:
            assert run.returncode==0,run.stdout+run.stderr
            print(run.stdout.strip())
