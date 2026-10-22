#!/usr/bin/env python3
"""Execute new C3 helper bodies with allocator failures and early wakeups."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]


def functions(file, names):
    source = (root/'kernel/linuxkpi/src'/file).read_text()
    result = ''
    for name in names:
        start = source.index(name)
        brace = source.index('{', start)
        depth, end = 1, brace + 1
        while depth:
            depth += (source[end] == '{') - (source[end] == '}')
            end += 1
        result += source[start:end] + '\n'
    return result


def run(name, source):
    with tempfile.TemporaryDirectory(prefix='nouveau-c3-') as tmp:
        path = Path(tmp)/name
        path.with_suffix('.c').write_text(source)
        subprocess.run(['cc','-std=gnu11','-Wall','-Wextra','-Werror',
                        '-fsanitize=address,undefined',str(path.with_suffix('.c')),
                        '-o',str(path)],check=True)
        subprocess.run([str(path)],check=True,timeout=10)


common = '#include <assert.h>\n#include <stdio.h>\n#include <stdbool.h>\n#include <stdlib.h>\n#include <stdint.h>\n#include <string.h>\n'
run('slab', common + r'''
typedef unsigned gfp_t;
struct kmem_cache { int unused; };
static int allocations,fail_at,live;
static unsigned last_flags;
static unsigned generic_frees;
static void kfree(void *p) { generic_frees++;free(p); }
static void *__kpi_kmem_cache_alloc(struct kmem_cache *c,gfp_t flags) {
 (void)c;last_flags=flags;if(++allocations==fail_at)return NULL;
 void *p=malloc(32);assert(p);live++;return p;
}
static void __kpi_kmem_cache_free(struct kmem_cache *c,void *p) { (void)c;if(p){live--;free(p);} }
static void *kmalloc(size_t n,gfp_t flags) { last_flags=flags;return fail_at?NULL:malloc(n); }
''' + functions('slab.c',['int kmem_cache_alloc_bulk(', 'void kmem_cache_free_bulk(', 'char *kstrndup(']) + r'''
int main(void) {
 struct kmem_cache cache={0};void *objects[8]={0};
 for(int failure=1;failure<=8;failure++) {
  allocations=0;fail_at=failure;
  assert(!kmem_cache_alloc_bulk(&cache,0x51,8,objects));
  assert(!live && last_flags==0x51);
  for(int i=0;i<failure;i++)assert(!objects[i]);
 }
 fail_at=0;allocations=0;
 assert(kmem_cache_alloc_bulk(&cache,0x73,8,objects)==8 && live==8 && last_flags==0x73);
 kmem_cache_free_bulk(&cache,8,objects);assert(!live);
 void *generic[2]={malloc(16),malloc(32)};kmem_cache_free_bulk(NULL,2,generic);assert(generic_frees==2);
 assert(!kmem_cache_alloc_bulk(NULL,0,1,objects));
 assert(!kmem_cache_alloc_bulk(&cache,0,SIZE_MAX,objects));
 assert(!kmem_cache_alloc_bulk(&cache,0,0,NULL));
 char *p=kstrndup("abcdef",3,9);assert(p && !strcmp(p,"abc") && last_flags==9);free(p);
 p=kstrndup("",10,2);assert(p && !p[0]);free(p);
 assert(!kstrndup(NULL,8,0));fail_at=1;assert(!kstrndup("abc",3,0));
 puts("PASS: bulk slab failure rollback/flags and bounded string duplication");
}
''')

run('dma', common + r'''
typedef uint64_t dma_addr_t;typedef uint64_t u64;typedef unsigned gfp_t;
enum dma_data_direction { DMA_BIDIRECTIONAL,DMA_TO_DEVICE,DMA_FROM_DEVICE,DMA_NONE };
#define DMA_MAPPING_ERROR UINT64_MAX
#define DMA_BIT_MASK(n) UINT64_MAX
#define ZERO_SIZE_PTR ((void *)16)
#define PAGE_SIZE 4096UL
#define PAGE_SHIFT 12
#define __GFP_ZERO 1
#define GFP_DMA32 2
struct device { u64 dma_mask,coherent_dma_mask; };
struct page { uint64_t phys; };
static struct page page;
static unsigned allocs,frees,flags_seen;
static bool ignore_low,fail_alloc;
static unsigned get_order(size_t n) { unsigned order=0;while((4096UL<<order)<n)order++;return order; }
static struct page *alloc_pages(gfp_t flags,unsigned order) {
 (void)order;allocs++;flags_seen=flags;
 if(fail_alloc)return NULL;
 page.phys=(flags&GFP_DMA32)&&!ignore_low?0x1000:0x100000000ULL;return &page;
}
static void __free_pages(struct page *p,unsigned order) { (void)order;assert(p==&page);frees++; }
static uint64_t page_to_phys(struct page *p) { return p->phys; }
static void *page_address(struct page *p) { return p; }
''' + functions('dma-mapping.c',['static bool kpi_dma_range_ok(', 'static void *kpi_dma_alloc(', 'void *dma_alloc_attrs(', 'void *dma_alloc_noncoherent(', 'void *dma_alloc_coherent(']) + r'''
int main(void) {
 struct device dev={UINT32_MAX,UINT64_MAX};dma_addr_t handle=0;
 assert(dma_alloc_noncoherent(&dev,4096,&handle,DMA_TO_DEVICE,0)==&page);
 assert(handle==0x1000 && (flags_seen&GFP_DMA32));
 assert(dma_alloc_coherent(&dev,4096,&handle,0)==&page && handle==0x100000000ULL);
 ignore_low=true;unsigned before=frees;
 assert(!dma_alloc_noncoherent(&dev,4096,&handle,DMA_TO_DEVICE,0) && frees==before+1);
 dev.dma_mask=UINT64_MAX;dev.coherent_dma_mask=UINT32_MAX;
 assert(dma_alloc_noncoherent(&dev,4096,&handle,DMA_FROM_DEVICE,0)==&page && handle==0x100000000ULL);
 assert(!dma_alloc_coherent(&dev,4096,&handle,0));
 before=allocs;assert(!dma_alloc_noncoherent(&dev,4096,&handle,DMA_NONE,0) && allocs==before);
 assert(!dma_alloc_noncoherent(&dev,SIZE_MAX,&handle,DMA_TO_DEVICE,0) && allocs==before);
 fail_alloc=true;assert(!dma_alloc_noncoherent(&dev,4096,&handle,DMA_TO_DEVICE,0));
 puts("PASS: noncoherent DMA uses streaming mask, coherent DMA keeps its mask, low zone and failure unwind");
}
''')

run('hrtimeout', common + r'''
#include <errno.h>
typedef int64_t ktime_t;
enum hrtimer_mode { HRTIMER_MODE_ABS=0,HRTIMER_MODE_REL=1 };
#define NSEC_PER_MSEC 1000000LL
#define TASK_RUNNING 0
static ktime_t now;
static unsigned long slept;
static int wakes,blocked,states;
static ktime_t ktime_get(void) { return now; }
static int linuxkpi_schedule_timeout_ms(unsigned long ms) { slept=ms;return wakes; }
static void schedule(void) { blocked++; }
#define __set_current_state(s) do {assert((s)==TASK_RUNNING);states++;}while(0)
''' + functions('time.c',['int schedule_hrtimeout(']) + r'''
int main(void) {
 ktime_t delay=1;assert(!schedule_hrtimeout(&delay,HRTIMER_MODE_REL) && slept==1);
 delay=1000001;assert(!schedule_hrtimeout(&delay,HRTIMER_MODE_REL) && slept==2);
 wakes=1;assert(schedule_hrtimeout(&delay,HRTIMER_MODE_REL)==-EINTR);
 wakes=0;now=4000000;delay=3000000;slept=0;
 assert(!schedule_hrtimeout(&delay,HRTIMER_MODE_ABS) && !slept);
 delay=5000001;assert(!schedule_hrtimeout(&delay,HRTIMER_MODE_ABS) && slept==2);
 delay=INT64_MIN;slept=0;assert(!schedule_hrtimeout(&delay,HRTIMER_MODE_ABS) && !slept);
 assert(schedule_hrtimeout(NULL,HRTIMER_MODE_REL)==-EINTR && blocked==1 && states==7);
 puts("PASS: hrtimer relative/absolute rounding, early wake, expired/null deadline and running-state restore");
}
''')

run('msi', common + r'''
#include <errno.h>
#define PCI_IRQ_MSI 2
struct pci_dev { unsigned irq,msi_enabled; };
struct kpi_pci_irq_state {
 bool legacy_msi;unsigned old_irq;bool used;void *native;unsigned count,irq[32];
};
static struct kpi_pci_irq_state kpi_pci_irqs[1];
static struct pci_dev *known;
static int allocation_error,released,unregistered;
static int kpi_pci_index_of(struct pci_dev *dev) { return dev==known?0:-1; }
static void linuxkpi_irq_unregister_mask(unsigned irq) { assert(irq==89);unregistered++; }
static void linuxkpi_native_pci_irq_release(void *ctx) { assert(ctx==(void *)32);released++; }
static int pci_alloc_irq_vectors(struct pci_dev *dev,unsigned min,unsigned max,unsigned flags) {
 assert(min==1 && max==1 && flags==PCI_IRQ_MSI);
 if(!dev || dev!=known)return -EINVAL;
 if(allocation_error)return allocation_error;
 if(kpi_pci_irqs[0].used)return -EBUSY;
 kpi_pci_irqs[0]=(struct kpi_pci_irq_state){.used=true,.native=(void *)32,.count=1,.irq={89}};
 return 1;
}
''' + functions('pci.c',['void pci_free_irq_vectors(', 'int pci_enable_msi(', 'void pci_disable_msi(']) + r'''
int main(void) {
 struct pci_dev dev={.irq=19};known=&dev;
 assert(pci_enable_msi(NULL)==-EINVAL);
 allocation_error=-ENOSPC;
 assert(pci_enable_msi(&dev)==-ENOSPC && dev.irq==19 && !dev.msi_enabled);
 allocation_error=0;
 assert(!pci_enable_msi(&dev) && dev.irq==89 && dev.msi_enabled);
 assert(pci_enable_msi(&dev)==-EBUSY && dev.irq==89 && dev.msi_enabled);
 pci_disable_msi(&dev);assert(dev.irq==19 && !dev.msi_enabled && released==1 && unregistered==1);
 pci_disable_msi(&dev);assert(released==1);
 assert(!pci_enable_msi(&dev));pci_free_irq_vectors(&dev);
 assert(dev.irq==19 && !dev.msi_enabled && released==2 && unregistered==2);
 puts("PASS: legacy MSI publishes its vector, preserves allocation errors and restores INTx identity on either free path");
}
''')
