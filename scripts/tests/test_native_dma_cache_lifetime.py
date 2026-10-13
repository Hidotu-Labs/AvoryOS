#!/usr/bin/env python3
"""Run the native DMA allocator against cache-typed reusable RAM."""
from pathlib import Path
import subprocess, sys, tempfile
root = Path(__file__).resolve().parents[2]
src = Path(sys.argv[1]) if len(sys.argv) > 1 else root/'kernel/src/mm/dma_alloc.c'
code = '\n'.join(x for x in src.read_text().splitlines() if not x.startswith('#include'))
code = code.replace('__asm__ volatile("clflush (%0)" :: "r"(virt + i) : "memory");', 'flush_line(virt + i);')
code = code.replace('__asm__ volatile("wbinvd" ::: "memory");', 'flush_all();')
driver = (root/'kernel/src/drivers/manager/udriver.c').read_text()
driver = driver[driver.index('static uint64_t driverctl_mmap('):driver.index('  uint8_t index = (uint8_t)(offset >> UDRV_MMAP_RESOURCE_SHIFT);')]
driver += '  return (uint64_t)-1;\n}\n'
driver_types = r'''
#define UDRIVER_MAX_DMA_BUFFERS 16
#define UDRIVER_MAX_MAPPINGS 32
#define UDRV_MMAP_TOKEN_SHIFT 48U
#define UDRV_MMAP_DMA_TAG (0x4000ULL<<48)
#define UDRV_MMAP_DMA_HANDLE_MASK 0x3fffU
#define UDRV_MMAP_DMA_INNER_MASK ((1ULL<<48)-1)
struct device {int dummy;};
struct udriver_dma_buffer {struct device *device;dma_buffer_t *buffer;uint32_t flags;uint16_t handle;};
struct udriver_session {
 struct udriver_dma_buffer dma[16];
 struct {struct device *device;uint64_t address,length;uint64_t *pml4;uint16_t dma_handle;} mappings[32];
};
typedef struct vfs_node {void *device;} vfs_node_t;
static bool session_is_owner(struct udriver_session *s){return s!=NULL;}
static void vmm_unmap_page(uint64_t *r,uint64_t va){(void)r;(void)va;}
'''
code += driver_types + driver
pre = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#define PAGE_SIZE 4096
#define PAGE_FLAG_PRESENT 1
#define PAGE_FLAG_RW 2
#define PAGE_FLAG_USER 4
#define PAGE_FLAG_PWT 8
#define PAGE_FLAG_PCD 16
#define PAGE_FLAG_NX (1ULL<<63)
#define DMA_FLAG_NOCACHE 4
#define DMA_FLAG_ANYWHERE 8
#define DMA_FLAG_LOW 2
#define DMA_FLAG_32BIT 1
#define SPINLOCK_INIT 0
typedef int spinlock_t;
typedef struct {void *virt;uint64_t phys;size_t size,pages;uint32_t flags;} dma_buffer_t;
void *dma_alloc_page_flags(uint32_t, uint64_t *);
void *dma_alloc_pages_flags(size_t, uint32_t, uint64_t *);
static unsigned char ram[8*PAGE_SIZE] __attribute__((aligned(4096)));
static uint64_t types[8],remote_types[8];
static bool irq_enabled=true,pending;
static uint64_t parked_phys;
static size_t parked_pages;
static unsigned flushed[8];
static bool owned, metadata_fail;
static int map_fail=-1,restore_fail=-1;
static size_t extent;
static unsigned allocations, releases;
static unsigned user_maps;
static void spinlock_acquire(spinlock_t *l){(void)l;assert(irq_enabled);irq_enabled=false;}
static void spinlock_release(spinlock_t *l){(void)l;irq_enabled=true;}
static bool hal_irq_enabled(void){return irq_enabled;}
static void klog_puts(const char *s){(void)s;}
static uint64_t pmm_get_hhdm_offset(void){return 0;}
static void *pmm_alloc_pages(size_t n){assert(!owned&&n<=8);owned=true;extent=n;allocations++;return ram;}
static void *pmm_alloc_pages_range(size_t n,uint64_t low,uint64_t high){(void)low;(void)high;return pmm_alloc_pages(n);}
static void pmm_free_pages(void *p,size_t n){
 assert(p==ram&&owned&&n==extent);
 /* Reusing this RAM for a WB user/TTM mapping must agree with HHDM. */
 for(size_t i=0;i<n;i++)assert(types[i]==0 && remote_types[i]==0);
 owned=false;releases++;
}
static void pmm_free_page(void *p){pmm_free_pages(p,1);}
static void *kmalloc(size_t n){return metadata_fail?NULL:malloc(n);}
static void kfree(void *p){free(p);}
static uint64_t *vmm_get_kernel_pml4(void){return (void *)1;}
static uint64_t *vmm_get_active_pml4(void){return (void *)1;}
static void vmm_flush_tlb(uint64_t v){(void)v;}
static void flush_line(uint64_t v){unsigned i=(v-(uintptr_t)ram)/PAGE_SIZE;assert(i<8);flushed[i]++;}
static void flush_all(void){for(unsigned i=0;i<8;i++)flushed[i]=PAGE_SIZE/64;}
static bool tlb_flush_deferred_drain(void){
 if(!pending)return true;
 if(!irq_enabled)return false;
 for(unsigned i=0;i<8;i++)remote_types[i]=types[i];
 pending=false;return true;
}
static void vmm_defer_frame_free(void *phys,uint16_t n){assert(!parked_pages);parked_phys=(uintptr_t)phys;parked_pages=n;}
static bool vmm_map_page(uint64_t *root,uint64_t va,uint64_t pa,uint64_t f){
 assert(root==(void *)1);unsigned i=(pa-(uintptr_t)ram)/PAGE_SIZE;
 assert(i<extent);uint64_t t=f&(PAGE_FLAG_PCD|PAGE_FLAG_PWT);
 if(va!=pa){assert(owned&&t==types[i]&&t==remote_types[i]);user_maps++;return true;}
 if(t&&map_fail==(int)i){map_fail=-1;return false;}
 if(!t&&restore_fail==(int)i)return false;
 /* Every successful type change must be preceded by line invalidation. */
 if(types[i]!=t)assert(flushed[i]>=PAGE_SIZE/64);
 flushed[i]=0;types[i]=t;pending=true;tlb_flush_deferred_drain();return true;
}
'''
post = r'''
int main(void){
 // Coherent WB allocations must work under an outer IRQ-masking lock without
 // creating any cache-mode invalidations or aliases on another CPU.
 for(int i=0;i<100;i++){
  irq_enabled=!(i&1);
  dma_buffer_t *b=dma_alloc(3*PAGE_SIZE-7,DMA_FLAG_ANYWHERE);assert(b&&b->pages==4);
  for(unsigned j=0;j<4;j++)assert(types[j]==0&&remote_types[j]==0);
  if(i==0){
   struct device device;struct udriver_session session={0};
   session.dma[0]=(struct udriver_dma_buffer){.device=&device,.buffer=b,.handle=1};
   vfs_node_t node={.device=&session};
   assert(driverctl_mmap(&node,0x10000,2*PAGE_SIZE,3,1,UDRV_MMAP_DMA_TAG|(1ULL<<48))==0x10000);
   assert(user_maps==2&&session.mappings[0].dma_handle==1);
  }
  assert(!pending);memset(b->virt,0x5a,b->pages*PAGE_SIZE);dma_free(b);assert(!owned);
 }
 irq_enabled=true;
 uint64_t phys;void *p=dma_alloc_page(&phys);assert(p==ram&&phys==(uintptr_t)ram);dma_free_page(p);assert(!owned);
 for(int i=0;i<100;i++){
  dma_buffer_t *b=dma_alloc(3*PAGE_SIZE-7,DMA_FLAG_ANYWHERE|DMA_FLAG_NOCACHE);assert(b&&b->pages==4);
  for(unsigned j=0;j<4;j++)assert(types[j]==24&&remote_types[j]==24);
  dma_free(b);assert(!owned&&!pending);
 }
 irq_enabled=false;unsigned before_alloc=allocations;
 assert(!dma_alloc(PAGE_SIZE,DMA_FLAG_ANYWHERE|DMA_FLAG_NOCACHE));assert(!owned&&allocations==before_alloc);
 irq_enabled=true;
 dma_buffer_t *b=dma_alloc(3*PAGE_SIZE,DMA_FLAG_ANYWHERE|DMA_FLAG_NOCACHE);assert(b);
 irq_enabled=false;unsigned before=releases;dma_free(b);
 assert(owned&&releases==before&&parked_pages==4&&pending);
 for(unsigned i=0;i<4;i++)assert(types[i]==0&&remote_types[i]==24);
 irq_enabled=true;assert(tlb_flush_deferred_drain());
 pmm_free_pages((void *)(uintptr_t)parked_phys,parked_pages);parked_pages=0;
 // The bare-page path retains the same safe release rule for explicit UC.
 p=dma_alloc_page_flags(DMA_FLAG_ANYWHERE|DMA_FLAG_NOCACHE,&phys);assert(p==ram);
 irq_enabled=false;dma_free_page(p);assert(owned&&parked_pages==1);
 irq_enabled=true;assert(tlb_flush_deferred_drain());pmm_free_pages(ram,parked_pages);parked_pages=0;
 metadata_fail=true;
 assert(!dma_alloc(3*PAGE_SIZE,DMA_FLAG_ANYWHERE));assert(!owned);
 assert(!dma_alloc(3*PAGE_SIZE,DMA_FLAG_ANYWHERE|DMA_FLAG_NOCACHE));assert(!owned);metadata_fail=false;
 for(int i=0;i<4;i++){map_fail=i;assert(!dma_alloc(3*PAGE_SIZE,DMA_FLAG_ANYWHERE|DMA_FLAG_NOCACHE));assert(!owned);}
 b=dma_alloc(3*PAGE_SIZE,DMA_FLAG_ANYWHERE|DMA_FLAG_NOCACHE);assert(b);
 restore_fail=1;before=releases;dma_free(b);assert(owned&&releases==before);
 restore_fail=-1;assert(dma_restore_pages((uintptr_t)ram,extent));dma_release_restored_pages((uintptr_t)ram,extent);
 assert(!dma_alloc(SIZE_MAX,DMA_FLAG_ANYWHERE));assert(!dma_alloc_pages_flags(SIZE_MAX,DMA_FLAG_ANYWHERE,NULL));
 assert(allocations==releases);
 puts("PASS: coherent DMA under masked IRQs, completed UC transitions, delayed remote TLBs, page/free/rollback/quarantine and overflow");
}
'''
with tempfile.TemporaryDirectory(prefix='native-dma-cache-') as tmp:
 if 'static bool dma_restore_pages(' not in code:
  code += '\nstatic bool dma_restore_pages(uint64_t p,size_t n){(void)p;(void)n;return false;}\n'
 if 'static void dma_release_restored_pages(' not in code:
  code += '\nstatic void dma_release_restored_pages(uint64_t p,size_t n){pmm_free_pages((void *)(uintptr_t)p,n);}\n'
 p=Path(tmp);(p/'test.c').write_text(pre+code+post)
 subprocess.run(['cc','-std=gnu11','-O2','-Wall','-Wextra','-Werror','-Wno-unused-function',str(p/'test.c'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
 if len(sys.argv) == 1:
  for name, broken in [
   ('reuse before remote acknowledgement', code.replace('if (tlb_flush_deferred_drain()) {', 'if (true) {')),
   ('missing pre-transition invalidation', code.replace('flush_line(virt + i);', '(void)(virt + i);')),
   ('UC user alias of coherent DMA RAM', code.replace('PAGE_FLAG_USER |\n                          PAGE_FLAG_NX;', 'PAGE_FLAG_USER | PAGE_FLAG_PWT | PAGE_FLAG_PCD |\n                          PAGE_FLAG_NX;')),
  ]:
   assert broken != code
   (p/'test.c').write_text(pre+broken+post)
   subprocess.run(['cc','-std=gnu11','-O2','-Wall','-Wextra','-Werror','-Wno-unused-function',str(p/'test.c'),'-o',str(p/'test')],check=True)
   result = subprocess.run([str(p/'test')],capture_output=True)
   assert result.returncode != 0, f'{name} escaped detection'
   print(f'PASS: {name} fails the negative control')
