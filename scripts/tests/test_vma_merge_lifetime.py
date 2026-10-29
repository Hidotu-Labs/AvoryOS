#!/usr/bin/env python3
"""Exercise actual VMA merging, AVL metadata, references and allocator failure."""
from pathlib import Path
import os
import re
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = Path(os.environ.get('VMA_SOURCE', root/'kernel/src/mm/vma.c')).read_text()
source = re.sub(r'^#include .*$', '', source, flags=re.M)
prefix = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "kernel/src/mm/vma.h"
#define TSC_BEGIN(x) ((void)0)
#define TSC_END(x) ((void)0)
static unsigned allocs, frees, fail_at;
static void *vma_cache;
static void *kmalloc(size_t size){if(fail_at && ++allocs>=fail_at)return NULL;if(!fail_at)allocs++;return malloc(size);}
static void kfree(void *p){if(p)frees++;free(p);}
static void *kmem_cache_alloc(void *c){(void)c;return kmalloc(sizeof(struct vma));}
static void kmem_cache_free(void *c,void *p){(void)c;kfree(p);}
typedef struct {unsigned refs,closes;} vfs_node_t;
static void vfs_node_ref(vfs_node_t *n){n->refs++;}
static void vfs_close(vfs_node_t *n){assert(n->refs);n->refs--;n->closes++;}
struct bridge {unsigned refs,closed;};
void linuxkpi_vma_ref(void *p){((struct bridge*)p)->refs++;}
void linuxkpi_vma_unref(void *p){struct bridge *b=p;assert(b->refs);if(!--b->refs)b->closed++;}
static void klog_puts(const char *s){(void)s;}
static void klog_hex64(uint64_t n){(void)n;}
static void klog_uint64(uint64_t n){(void)n;}
'''
tests = r'''
static int validate(struct vma *v,uint64_t *last,int *count){
 if(!v)return 0;
 int l=validate(v->left,last,count);assert(v->start>=*last);assert(v->end>v->start);*last=v->end;(*count)++;
 int r=validate(v->right,last,count);assert(abs(l-r)<=1);assert(v->height==1+(l>r?l:r));
 uint64_t max=v->end;if(v->left&&v->left->max_end>max)max=v->left->max_end;if(v->right&&v->right->max_end>max)max=v->right->max_end;assert(v->max_end==max);return v->height;
}
static void check(struct vma_list *l){uint64_t last=0;int count=0;validate(l->root,&last,&count);assert(count==l->count);}
int main(void){
 struct vma_list l;vma_list_init(&l);
 vfs_node_t file={0};struct bridge bridge={0};
 /* Preserve a file/driver mapping across unrelated anonymous merges. */
 assert(!vma_add(&l,0x1000,0x2000,1,MAP_SHARED,17,123,&file,999));
 vma_attach_linux(&l,0x1000,&bridge);struct vma *saved=vma_find(&l,0x1000);saved->numa_policy=2;saved->numa_nodemask=8;
 for(unsigned i=0;i<512;i++)assert(!vma_add(&l,0x10000+i*4096,0x11000+i*4096,3,MAP_PRIVATE|MAP_ANONYMOUS,-1,0,NULL,0));
 unsigned before=allocs;
 /* One allocation for the pointer snapshot is permitted. No node allocation
  * may occur after destructive work begins, even if the allocator fails. */
 fail_at=allocs+3;vma_merge_adjacent(&l);fail_at=0;
 assert(l.count==2);assert(allocs-before==1);assert(vma_find(&l,0x1000)==saved);
 assert(saved->fd==17&&saved->offset==123&&saved->file_size==999&&saved->numa_policy==2&&saved->numa_nodemask==8);
 assert(file.refs==1&&!file.closes&&bridge.refs==1&&!bridge.closed);check(&l);
 assert(vma_find(&l,0x20f000));assert(!vma_find(&l,0x210000));
 vma_list_destroy(&l);assert(!file.refs&&file.closes==1&&!bridge.refs&&bridge.closed==1);
 /* Nodes sharing a bridge drop only the redundant node's reference. */
 bridge=(struct bridge){0};vma_list_init(&l);
 for(unsigned i=0;i<8;i++){uint64_t a=0x10000+i*4096;assert(!vma_add(&l,a,a+4096,3,MAP_PRIVATE|MAP_ANONYMOUS,-1,0,NULL,0));vma_attach_linux(&l,a,&bridge);}
 assert(bridge.refs==8);vma_merge_adjacent(&l);assert(l.count==1&&bridge.refs==1&&!bridge.closed);check(&l);
 vma_list_destroy(&l);assert(!bridge.refs&&bridge.closed==1);
 /* An initial snapshot failure is harmless and leaves every cached node live. */
 vma_list_init(&l);for(unsigned i=0;i<128;i++)assert(!vma_add(&l,0x10000+i*4096,0x11000+i*4096,3,MAP_PRIVATE|MAP_ANONYMOUS,-1,0,NULL,0));
 saved=vma_find(&l,0x10000);fail_at=allocs+1;vma_merge_adjacent(&l);fail_at=0;assert(l.count==128&&vma_find(&l,0x10000)==saved);check(&l);
 vma_merge_adjacent(&l);assert(l.count==1);check(&l);vma_list_destroy(&l);
 /* Interleaved policies and protected regions remain distinct. */
 vma_list_init(&l);for(unsigned i=0;i<300;i++){assert(!vma_add(&l,0x10000+i*4096,0x11000+i*4096,i%3,MAP_PRIVATE|MAP_ANONYMOUS,-1,0,NULL,0));}
 vma_merge_adjacent(&l);assert(l.count==300);check(&l);
 for(unsigned i=0;i<100;i++){uint64_t a=0x10000+i*3*4096;assert(!vma_mprotect(&l,a,a+3*4096,3));vma_merge_adjacent(&l);check(&l);}
 assert(l.count==1);vma_list_destroy(&l);
 puts("PASS: real VMA merge preserves mappings/references, AVL metadata and lookups; OOM cannot destroy mappings");
}
'''
with tempfile.TemporaryDirectory() as tmp:
    path = Path(tmp)
    (path/'test.c').write_text(prefix+source+tests)
    subprocess.run(['cc', '-std=gnu11', '-O1', '-g', '-fsanitize=address,undefined',
                    '-I', str(root), str(path/'test.c'), '-o', str(path/'test')], check=True)
    subprocess.run([str(path/'test')], check=True, env=dict(os.environ, ASAN_OPTIONS='detect_leaks=0'))
