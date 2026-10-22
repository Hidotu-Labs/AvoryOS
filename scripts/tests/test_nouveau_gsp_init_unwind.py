#!/usr/bin/env python3
"""Execute actual GSP init failure cleanup without faking running=true."""
from pathlib import Path
import subprocess,tempfile
ROOT=Path(__file__).resolve().parents[2]
source=(ROOT/'kernel/linux/drivers/gpu/drm/nouveau/nvkm/subdev/gsp/r535.c').read_text()
start=source.index('static void\nr535_gsp_abort_init(');end=source.index('\nstatic int\n',start)
functions=source[start:end]
shim=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
typedef uint32_t u32;
struct nvkm_device { int unused; };
struct nvkm_subdev { struct nvkm_device *device; };
struct mem { void *data; uint64_t addr; };
struct nvkm_gsp { struct nvkm_subdev subdev; int falcon; bool running; struct mem wpr_meta; struct { struct mem meta; int radix3,sgt; } sr; };
static char trace[32];static unsigned cursor;
static int boot_result,poll_result,post_result,cleanup_result;
static void event(char c) { trace[cursor++]=c;trace[cursor]=0; }
#define WARN_ON(x) (x)
#define lower_32_bits(x) ((u32)(x))
#define upper_32_bits(x) ((u32)((x)>>32))
#define NV_VGPU_MSG_EVENT_GSP_INIT_DONE 4097
#define nvkm_info(...) ((void)reset,(void)fwsec,(void)unload)
static int nvkm_falcon_reset(int *f) { (void)f;event('R');return cleanup_result; }
static int nvkm_gsp_fwsec_sb(struct nvkm_gsp *g) { assert(!g->running);event('S');return cleanup_result; }
static int r535_gsp_booter_unload(struct nvkm_gsp *g,u32 a,u32 b) { assert(!g->running && a==255 && b==255);event('U');return cleanup_result; }
static int r535_gsp_booter_load(struct nvkm_gsp *g,u32 a,u32 b) { (void)g;assert(a==0x11223344 && b==0x55667788);event('B');return boot_result; }
static int r535_gsp_rpc_poll(struct nvkm_gsp *g,unsigned fn) { (void)g;assert(fn==4097);event('P');return poll_result; }
static void r535_gsp_log_dump(struct nvkm_gsp *g) { assert(!g->running);event('L'); }
static int r535_gsp_postinit(struct nvkm_gsp *g) { assert(g->running);event('I');return post_result; }
static void r535_gsp_rmargs_init(struct nvkm_gsp *g,bool resume) { (void)g;assert(resume); }
static void nvkm_gsp_mem_dtor(struct nvkm_gsp *g,struct mem *m) { (void)g;(void)m;event('M'); }
static void nvkm_gsp_radix3_dtor(struct nvkm_gsp *g,int *r) { (void)g;(void)r;event('X'); }
static void nvkm_gsp_sg_free(struct nvkm_device *d,int *s) { (void)d;(void)s;event('F'); }
'''
main=r'''
static void clear(void) { cursor=0;trace[0]=0;boot_result=poll_result=post_result=cleanup_result=0; }
int main(void) {
 struct nvkm_gsp g={0};g.wpr_meta.addr=0x5566778811223344ULL;
 clear();poll_result=-22;assert(r535_gsp_init(&g)==-22 && !g.running);assert(!strcmp(trace,"BPLRSU"));
 clear();poll_result=-22;cleanup_result=-5;assert(r535_gsp_init(&g)==-22 && !g.running);assert(!strcmp(trace,"BPLRSU"));
 clear();boot_result=-5;assert(r535_gsp_init(&g)==-5 && !g.running);assert(!strcmp(trace,"BL"));
 clear();assert(!r535_gsp_init(&g) && g.running);assert(!strcmp(trace,"BPI"));
 g.running=false;clear();post_result=-12;assert(r535_gsp_init(&g)==-12 && g.running);assert(!strcmp(trace,"BPI"));
 g.running=false;g.sr.meta.data=&g;g.sr.meta.addr=g.wpr_meta.addr;clear();poll_result=-22;
 assert(r535_gsp_init(&g)==-22 && !g.running);assert(!strcmp(trace,"BPLRSUMXF"));
}
'''
with tempfile.TemporaryDirectory(prefix='nouveau-init-unwind-') as tmp:
 exe=Path(tmp)/'test';exe.with_suffix('.c').write_text(shim+functions+main)
 subprocess.run(['cc','-std=gnu11','-Wall','-Wextra','-Werror',str(exe.with_suffix('.c')),'-o',str(exe)],check=True)
 subprocess.run([str(exe)],check=True)
print('PASS: failed INIT_DONE dumps logs then reset/FWSEC/unload before freeing DMA; original error preserved; early boot/success/postinit/resume paths')
