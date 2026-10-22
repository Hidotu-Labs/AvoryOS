#!/usr/bin/env python3
"""Execute the TTM eviction entry point with empty, busy and fenced managers."""
from pathlib import Path
import re,subprocess,tempfile
root=Path(__file__).resolve().parents[2]
original=(root/'kernel/linux/drivers/gpu/drm/ttm/ttm_resource.c').read_text()
a=original.index('int ttm_resource_manager_evict_all(')
b=original.index('EXPORT_SYMBOL',a)
old=original[a:b]
shim=r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <errno.h>
#define TTM_MAX_BO_PRIORITY 3
struct list_head {int entries;};
static bool list_empty(struct list_head*h){return h->entries==0;}
struct dma_fence {int refs;};
struct ttm_device {int lru_lock;};
struct ttm_resource_manager {struct list_head lru[3];int move_lock;struct dma_fence *move;};
struct ttm_operation_ctx {bool interruptible,no_wait_gpu,force_alloc;};
static int locked,evictions,evict_error,waits,wait_error;
static void spin_lock(int*l){(void)l;assert(!locked++);}
static void spin_unlock(int*l){(void)l;assert(locked--==1);}
static int ttm_mem_evict_first(struct ttm_device*b,struct ttm_resource_manager*m,void*p,struct ttm_operation_ctx*c,void*x){
 (void)b;(void)p;(void)x;assert(!locked&&!c->interruptible&&!c->no_wait_gpu&&c->force_alloc);
 if(evict_error)return evict_error;
 for(int i=0;i<3;i++)if(m->lru[i].entries){m->lru[i].entries--;evictions++;return 0;}
 return -ENOENT;
}
static struct dma_fence *dma_fence_get(struct dma_fence*f){assert(locked);if(f)f->refs++;return f;}
static void dma_fence_put(struct dma_fence*f){assert(!locked&&f->refs>1);f->refs--;}
static int dma_fence_wait(struct dma_fence*f,bool interruptible){assert(f&&!interruptible&&!locked);waits++;return wait_error;}
'''
modern = 'ttm_bo_evict_first(' in old
if modern:
 shim += r'''
static int ttm_bo_evict_first(struct ttm_device*b,struct ttm_resource_manager*m,struct ttm_operation_ctx*c) {
 return ttm_mem_evict_first(b,m,NULL,c,NULL);
}
#define cond_resched() ((void)0)
'''
shim += '#define KERNEL_VERSION(a,b,c) (((a)<<16)|((b)<<8)|(c))\n'
shim += '#define LINUX_VERSION_CODE '+('396288' if modern else '394752')+'\n'
main=r'''
int main(void){
 struct ttm_device b={0};struct ttm_resource_manager m={0};
 assert(!ttm_resource_manager_evict_all(&b,&m)&&!evictions&&!waits&&!locked);
 struct dma_fence f={.refs=1};m.move=&f;
 assert(!ttm_resource_manager_evict_all(&b,&m)&&waits==1&&f.refs==1);
 wait_error=-EIO;assert(ttm_resource_manager_evict_all(&b,&m)==-EIO&&f.refs==1&&!locked);wait_error=0;
 m.lru[0].entries=2;m.lru[2].entries=3;
 assert(!ttm_resource_manager_evict_all(&b,&m)&&evictions==5&&!m.lru[0].entries&&!m.lru[2].entries);
 m.lru[1].entries=1;evict_error=-EBUSY;int before=waits;
 assert(ttm_resource_manager_evict_all(&b,&m)==-EBUSY&&m.lru[1].entries==1&&waits==before&&!locked&&f.refs==1);
 puts("PASS: empty and occupied TTM managers, move fence drain, error propagation and lock/ref balance");
}
'''
with tempfile.TemporaryDirectory(prefix='kpi-ttm-evict-') as tmp:
 p=Path(tmp)
 shim_source=re.sub(r'^#include.*$','',(root/'kernel/linuxkpi/src/ttm_eviction.c').read_text(),flags=re.M)
 if modern:
  sources=[('fixed',old.replace('int ttm_resource_manager_evict_all(', 'int linuxkpi_original_ttm_resource_manager_evict_all(',1)+shim_source)]
 else:
  sources=[('old',old),('fixed',shim_source)]
 for name,source in sources:
  (p/(name+'.c')).write_text(shim+source+main)
  subprocess.run(['cc','-std=gnu11','-O0','-g','-ftrivial-auto-var-init=pattern','-fsanitize=address,undefined',str(p/(name+'.c')),'-o',str(p/name)],check=True)
  r=subprocess.run([str(p/name)],capture_output=True,text=True,timeout=10)
  if name=='old':
   assert r.returncode!=0,'old uninitialized result unexpectedly passed'
   assert 'Assertion' in r.stderr,r.stderr
   print('PASS: imported uninitialized empty-manager result fails the same regression')
  else:
   assert r.returncode==0,r.stdout+r.stderr
   print(r.stdout.strip())
   if modern: print('PASS: modern shim delegates to the corrected imported eviction algorithm')
