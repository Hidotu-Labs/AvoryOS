#!/usr/bin/env python3
"""Exercise native maple-tree allocation reservations, including failures."""
from pathlib import Path
import subprocess
import tempfile
ROOT=Path(__file__).resolve().parents[2]
source=(ROOT/'kernel/linuxkpi/src/slab.c').read_text()
a=source.index('struct slab_sheaf {');b=source.index('/* ── kmalloc family',a)
shim = r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
typedef int gfp_t;
struct kmem_cache { unsigned live; };
static int fail_after=-1;
static unsigned batches;
static void *kmalloc(size_t n,gfp_t g){(void)g;void*p=malloc(n);if(p)batches++;return p;}
static void kfree(void*p){if(p){assert(batches);batches--;free(p);}}
static void *kmem_cache_alloc(struct kmem_cache*c,gfp_t g){(void)g;if(!fail_after)return NULL;if(fail_after>0)fail_after--;void*p=malloc(32);if(p)c->live++;return p;}
static void kmem_cache_free(struct kmem_cache*c,void*p){assert(p&&c->live);c->live--;free(p);}
'''
main = r'''
int main(void){
 struct kmem_cache a={0},b={0};
 struct slab_sheaf*s=kmem_cache_prefill_sheaf(&a,0,8);assert(s&&a.live==8&&batches==1);
 void*p[8];for(int i=0;i<8;i++){p[i]=kmem_cache_alloc_from_sheaf(&a,0,s);assert(p[i]);for(int j=0;j<i;j++)assert(p[i]!=p[j]);}
 assert(!kmem_cache_sheaf_size(s));
 for(int i=0;i<8;i++)kmem_cache_free(&a,p[i]);
 assert(!kmem_cache_refill_sheaf(&a,0,&s,2)&&a.live==2);
 assert(kmem_cache_refill_sheaf(&b,0,&s,4)==-EINVAL&&a.live==2);
 fail_after=2;assert(kmem_cache_refill_sheaf(&a,0,&s,8)==-ENOMEM);
 assert(kmem_cache_sheaf_size(s)==4&&a.live==4);
 kmem_cache_return_sheaf(&a,0,s);assert(!a.live&&!batches);
 assert(!kmem_cache_prefill_sheaf(&a,0,8));assert(!a.live&&!batches);
 fail_after=-1;s=kmem_cache_prefill_sheaf(&a,0,0);assert(s);kmem_cache_return_sheaf(&a,0,s);
 assert(!batches&&!a.live&&!b.live);
 puts("PASS: actual sheaf ownership, batch consumption, resize, wrong-cache rejection, partial refill failure and complete rollback");
}
'''
with tempfile.TemporaryDirectory(prefix='kpi-sheaf-') as tmp:
    p=Path(tmp)/'test.c';p.write_text(shim+source[a:b]+main)
    exe=Path(tmp)/'test'
    subprocess.run(['cc','-std=gnu11','-O1','-g','-fsanitize=address,undefined',str(p),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True,timeout=15)
