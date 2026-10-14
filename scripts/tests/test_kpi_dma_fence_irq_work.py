#!/usr/bin/env python3
"""Run imported fence-array callbacks on actual irq_work with final-ref free."""
from pathlib import Path
import ast,re,subprocess,tempfile
root=Path(__file__).resolve().parents[2]
# Reuse the single-worker CPU shim, preserving the production queue/worker.
tree=ast.parse((root/'scripts/tests/test_kpi_irq_work_lifetime.py').read_text())
shim=next(ast.literal_eval(n.value) for n in tree.body if isinstance(n,ast.Assign) and any(isinstance(t,ast.Name) and t.id=='shim' for t in n.targets))
s=(root/'kernel/linuxkpi/src/irq_work.c').read_text().split('void irq_work_sync(')[0]
s=re.sub(r'^#include.*$','',s,flags=re.M)
f=(root/'kernel/linux/drivers/dma-buf/dma-fence-array.c').read_text()
def fn(sig):
 a=f.index(sig);end=f.index('{',a)+1;depth=1
 while depth:
  depth+=(f[end]=='{')-(f[end]=='}');end+=1
 return f[a:end]+'\n'
code=shim+s+r'''
#include <stddef.h>
#define container_of(p,t,m) ((t*)((char*)(p)-offsetof(t,m)))
#define PENDING_ERROR 1
#define cmpxchg(p,old,new) ({__typeof__(*(p)) expect=(old);__atomic_compare_exchange_n(p,&expect,(new),false,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE);expect;})
struct dma_fence {int error,refs,signaled;};
struct dma_fence_array {struct dma_fence base;int num_pending;struct irq_work work;};
struct dma_fence_cb {int unused;};
struct dma_fence_array_cb {struct dma_fence_cb cb;struct dma_fence_array *array;};
static unsigned signals,frees;
static void dma_fence_signal(struct dma_fence*f){assert(!f->signaled);f->signaled=1;signals++;}
static void dma_fence_put(struct dma_fence*f){assert(f->refs>0);if(!--f->refs){assert(f->signaled);frees++;free(f);}}
static bool atomic_dec_and_test(int*p){return --*p==0;}
'''+''.join(fn(sig) for sig in ['static void dma_fence_array_set_pending_error(', 'static void dma_fence_array_clear_pending_error(', 'static void irq_dma_fence_array_work(', 'static void dma_fence_array_cb_func('])+r'''
int main(void){
 for(int i=0;i<2000;i++){
  struct dma_fence_array*a=calloc(1,sizeof(*a));assert(a);a->base.refs=1;a->base.error=PENDING_ERROR;a->num_pending=1;
  init_irq_work(&a->work,irq_dma_fence_array_work);
  struct dma_fence_array_cb cb={.array=a};struct dma_fence child={.error=(i&1)?-5:0};
  dma_fence_array_cb_func(&child,&cb.cb);assert(a->work.queued&&a->base.refs==1);
  assert(!irq_work_kthread(NULL)); /* production callback drops final ref */
  assert(!irq_work_head&&!irq_work_tail&&!irq_work_current);
 }
 assert(signals==2000&&frees==2000);
 puts("PASS: 2000 imported fence-array completions drop final reference safely under actual irq_work (ASan/UBSan)");
}
'''
with tempfile.TemporaryDirectory(prefix='kpi-fence-irqwork-') as tmp:
 p=Path(tmp);(p/'test.c').write_text(code)
 subprocess.run(['cc','-std=gnu11','-O1','-g','-fsanitize=address,undefined',str(p/'test.c'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True,timeout=10)
