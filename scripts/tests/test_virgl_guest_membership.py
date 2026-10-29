#!/usr/bin/env python3
"""Run guest Mesa's actual resource index with real Mesa sets and injected OOM."""
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[2]
base=root/'build/mesa-virgl/mesa-26.2.5'
s=(base/'src/gallium/winsys/virgl/drm/virgl_drm_winsys.c').read_text()
code=s[s.index('static bool virgl_drm_alloc_res_list('):s.index('static void virgl_drm_emit_res(')]
pre=r'''
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <time.h>
struct set;struct set_entry;
extern struct set *_mesa_pointer_set_create(void*);
extern struct set_entry *_mesa_set_add(struct set*,const void*);
extern struct set_entry *_mesa_set_search(const struct set*,const void*);
extern void _mesa_set_destroy(struct set*,void(*)(struct set_entry*));
extern void _mesa_set_clear(struct set*,void(*)(struct set_entry*));
static bool fail_create,fail_add,fail_realloc;
static struct set *index_create(void *p){if(fail_create)return NULL;return _mesa_pointer_set_create(p);}
static struct set_entry *index_add(struct set*s,const void*p){if(fail_add){fail_add=false;return NULL;}return _mesa_set_add(s,p);}
#define _mesa_pointer_set_create index_create
#define _mesa_set_add index_add
#define CALLOC(n,s) calloc(n,s)
#define MALLOC(n) malloc(n)
#define FREE(p) free(p)
static void *resize(void*p,size_t old,size_t n){(void)old;if(fail_realloc){fail_realloc=false;return NULL;}return realloc(p,n);}
#define REALLOC(p,o,n) resize(p,o,n)
#define unlikely(x) (x)
#define _debug_printf(...) ((void)0)
#define p_atomic_inc(p) (++*(p))
#define p_atomic_dec(p) (--*(p))
#define p_atomic_set(p,v) (*(p)=(v))
struct virgl_winsys {int unused;};
struct virgl_drm_winsys {struct virgl_winsys base;};
struct virgl_hw_res {unsigned refs,num_cs_references,bo_handle;bool maybe_busy;};
struct virgl_drm_cmd_buf {unsigned nres,cres;struct virgl_hw_res **res_bo;struct virgl_winsys *ws;uint32_t *res_hlist;struct set *res_set;};
static void virgl_drm_resource_reference(struct virgl_winsys*w,struct virgl_hw_res **dst,struct virgl_hw_res*src){(void)w;if(src)src->refs++;if(*dst){assert((*dst)->refs>1);(*dst)->refs--;}*dst=src;}
static bool linear(struct virgl_drm_cmd_buf*c,struct virgl_hw_res*r){for(unsigned i=0;i<c->cres;i++)if(c->res_bo[i]==r)return true;return false;}
'''
main=r'''
int main(void){enum{N=6000};struct virgl_hw_res *bo=calloc(N+1,sizeof(*bo));assert(bo);for(unsigned i=0;i<=N;i++){bo[i].refs=1;bo[i].bo_handle=i+1;}
 struct virgl_drm_winsys ws={0};struct virgl_drm_cmd_buf cb={.ws=&ws.base};
 assert(virgl_drm_alloc_res_list(&cb,512)&&cb.res_set);
 for(unsigned repeat=0;repeat<4;repeat++)for(unsigned i=0;i<N;i++)virgl_drm_add_res(&ws,&cb,&bo[i]);
 assert(cb.cres==N&&!virgl_drm_res_is_added(&cb,&bo[N]));
 for(unsigned i=0;i<N;i++)assert(bo[i].refs==2&&bo[i].num_cs_references==1&&cb.res_hlist[i]==i+1);
 volatile unsigned found=0;double elapsed[2];
 for(unsigned mode=0;mode<2;mode++){clock_t start=clock();for(unsigned r=0;r<8;r++)for(unsigned i=0;i<N;i++)found+=mode?virgl_drm_res_is_added(&cb,&bo[i]):linear(&cb,&bo[i]);elapsed[mode]=(double)(clock()-start)/CLOCKS_PER_SEC;}
 assert(found==N*16);printf("LOOKUP: 6000 BOs x 8 passes linear=%.3f ms indexed=%.3f ms\n",1000*elapsed[0],1000*elapsed[1]);
 struct virgl_drm_cmd_buf other={.ws=&ws.base};assert(virgl_drm_alloc_res_list(&other,512));virgl_drm_add_res(&ws,&other,&bo[0]);assert(bo[0].refs==3&&bo[0].num_cs_references==2);
 virgl_drm_clear_res_list(&cb);assert(!cb.cres&&!virgl_drm_res_is_added(&cb,&bo[0])&&virgl_drm_res_is_added(&other,&bo[0]));assert(bo[0].refs==2&&bo[0].num_cs_references==1);
 virgl_drm_free_res_list(&other);assert(bo[0].refs==1&&!bo[0].num_cs_references);
 for(unsigned i=0;i<N;i++)assert(bo[i].refs==1&&!bo[i].num_cs_references&&bo[i].maybe_busy);
 virgl_drm_add_res(&ws,&cb,&bo[0]);fail_add=true;virgl_drm_add_res(&ws,&cb,&bo[1]);assert(!cb.res_set&&cb.cres==2);virgl_drm_add_res(&ws,&cb,&bo[0]);assert(cb.cres==2&&virgl_drm_res_is_added(&cb,&bo[1]));virgl_drm_free_res_list(&cb);
 fail_create=true;cb=(struct virgl_drm_cmd_buf){.ws=&ws.base};assert(virgl_drm_alloc_res_list(&cb,512)&&!cb.res_set);fail_create=false;
 for(unsigned i=0;i<512;i++)virgl_drm_add_res(&ws,&cb,&bo[i]);fail_realloc=true;virgl_drm_add_res(&ws,&cb,&bo[512]);assert(cb.cres==512&&!virgl_drm_res_is_added(&cb,&bo[512])&&bo[512].refs==1);
 virgl_drm_add_res(&ws,&cb,&bo[512]);assert(cb.cres==513&&virgl_drm_res_is_added(&cb,&bo[512]));virgl_drm_free_res_list(&cb);
 for(unsigned i=0;i<=N;i++)assert(bo[i].refs==1&&!bo[i].num_cs_references);free(bo);
 puts("PASS: duplicate references, 6000 BOs, growth, clear/reuse, independent command buffers, hash creation/growth OOM and resource-array OOM preserve ownership");}
'''
with tempfile.TemporaryDirectory(prefix='virgl-guest-membership-') as tmp:
 p=Path(tmp);(p/'test.c').write_text(pre+code+main)
 subprocess.run(['cc','-O2','-Wall','-Wextra','-Werror','-Wno-misleading-indentation','-Wno-sign-compare',str(p/'test.c'),str(root/'build/mesa-virgl/obj/src/util/libmesa_util.a'),'-lm','-lpthread','-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
