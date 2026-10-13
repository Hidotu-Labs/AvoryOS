#!/usr/bin/env python3
"""Run memfd's actual mapper and CoW copy against live backing frames."""
from pathlib import Path
import subprocess
import tempfile
root = Path(__file__).resolve().parents[2]
def function(source, signature):
    start = source.index(signature)
    end = source.index('{', start) + 1
    depth = 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end] + '\n'
source = (root/'kernel/src/syscalls/sys_aio.c').read_text()
code = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#define PAGE_SIZE 4096
#define MAP_PRIVATE 2
#define MAP_SHARED 1
#define MAP_ANONYMOUS 32
#define MAP_PAGECACHE (1ULL<<34)
#define TEARDOWN_COLLECT 64
#define PAGE_FLAG_PRESENT 1ULL
#define PAGE_FLAG_RW 2ULL
#define PAGE_FLAG_USER 4ULL
#define PAGE_FLAG_COW 512ULL
#define PAGE_FLAG_REFAULT 1024ULL
#define PAGE_FLAG_NX (1ULL<<63)
#define PAGE_MASK 0x000ffffffffff000ULL
#define PHYS_TO_VIRT(p) ((void *)(uintptr_t)(p))
typedef struct {uint8_t *data;uint32_t capacity;uint8_t data_is_pmm;} ramfs_file_t;
typedef struct {void *device;uint32_t length;bool mmap_private_pmm_refs;} vfs_node_t;
struct vma {uint64_t start,end,flags;int fd;void *file_node;};
struct mm {struct vma *vmas;};
struct thread {struct mm *mm;};
static struct vma views[5];
static struct mm address_space={views};
static struct vma *vma_find(struct vma **tree,uint64_t va){
 for(int i=0;i<5;i++)if(va>=(*tree)[i].start && va<(*tree)[i].end)return &(*tree)[i];
 return NULL;
}
static struct thread current={&address_space};
static struct thread *sched_get_current(void){return &current;}
static uint64_t pmm_get_hhdm_offset(void){return 0;}
static uint64_t *vmm_get_active_pml4(void){return NULL;}
static uint64_t mm_alloc_mmap_region(uint64_t n){(void)n;return 0x100000;}
static struct {uint64_t phys;unsigned refs;} frames[128];
static int frame_count;
static void *allocations[128];
static int allocation_count;
static void *pmm_alloc_pages(size_t n){
 void *p=aligned_alloc(PAGE_SIZE,n*PAGE_SIZE);assert(p);
 allocations[allocation_count++]=p;
 for(size_t i=0;i<n;i++)frames[frame_count++]=(typeof(frames[0])){(uint64_t)p+i*PAGE_SIZE,1};
 return p;
}
static unsigned *ref(void *p){
 for(int i=0;i<frame_count;i++)if(frames[i].phys==(uint64_t)p)return &frames[i].refs;
 assert(0);return NULL;
}
static void pmm_incref(void *p){assert(*ref(p));++*ref(p);}
static void pmm_decref(void *p){assert(*ref(p));--*ref(p);}
static uint16_t pmm_get_ref(void *p){return *ref(p);}
static void ramfs_free_file_data(ramfs_file_t *f){
 for(unsigned i=0;i<f->capacity/PAGE_SIZE;i++)pmm_decref(f->data+i*PAGE_SIZE);
 f->data=NULL;f->capacity=0;
}
static struct {uint64_t va,pte;} mappings[128];
static int map_count;
static bool vmm_map_page(uint64_t *p,uint64_t va,uint64_t phys,uint64_t flags){
 (void)p;assert(map_count<128);mappings[map_count++]=(typeof(mappings[0])){va,phys|flags};return true;
}
static uint64_t *mapping(uint64_t va){
 for(int i=0;i<map_count;i++)if(mappings[i].va==va)return &mappings[i].pte;
 assert(0);return NULL;
}
static void *vmm_alloc_page_numa(int a,int b,int c,uint64_t d){
 (void)a;(void)b;(void)c;(void)d;return pmm_alloc_pages(1);
}
static void page_copy(void *a,void *b){memcpy(a,b,PAGE_SIZE);}
#define WALK_REJECT(...) assert(0)
static void vmm_lock_acquire(void){}
static void vmm_lock_release(void){}
static bool tlb_flush_deferred_drain(void){return true;}
static void vmm_defer_frame_free(void *p,size_t n){(void)p;(void)n;assert(0);}
static void pmm_free_page(void *p){pmm_decref(p);}
static void pmm_free_pages(void *p,size_t n){for(size_t i=0;i<n;i++)pmm_decref((char *)p+i*PAGE_SIZE);}
static void vmm_free_empty_tables(uint64_t *p,uint64_t va){(void)p;(void)va;}
static void vmm_drain_deferred_work(void){}
static bool vmm_split_huge_page(uint64_t *p,uint64_t va){(void)p;(void)va;assert(0);return false;}
static void vmm_unmap_huge_locked(uint64_t *p,uint64_t va,uint64_t *phys){(void)p;(void)va;(void)phys;assert(0);}
typedef enum {VMM_UNMAP_HOLE,VMM_UNMAP_4K,VMM_UNMAP_HUGE} vmm_unmap_kind_t;
static vmm_unmap_kind_t vmm_unmap_page_locked(uint64_t *p,uint64_t va,uint64_t *phys){
 (void)p;uint64_t *pte=mapping(va);
 if(!*pte)return VMM_UNMAP_HOLE;
 *phys=*pte&PAGE_MASK;*pte=0;return VMM_UNMAP_4K;
}
'''
code += function(source, 'static uint64_t memfd_mmap(')
mm_source = (root/'kernel/src/syscalls/sys_mm.c').read_text()
for signature in ['static bool vma_has_file_page_refs(', 'static void teardown_release_collected(', 'static void teardown_range_locked(']:
    code += function(mm_source, signature)

# Execute the real reference-count/copy part of the fault handler.
fault = (root/'kernel/src/mm/vmm_fault.c').read_text()
start = fault.index('      uint16_t refs = pmm_get_ref((void *)old_phys);')
end = fault.index('      pml4[(virt >> 39)',start)
code += '''static void private_write(uint64_t va,uint8_t value){
 uint64_t *pte=mapping(va), old_phys=*pte&PAGE_MASK, virt=va, cow_old_phys=0;
 assert((*pte&PAGE_FLAG_COW) && !(*pte&PAGE_FLAG_RW));
''' + fault[start:end] + r'''
 if(cow_old_phys)pmm_decref((void *)cow_old_phys);
 ((uint8_t *)(uintptr_t)(*pte&PAGE_MASK))[0]=value;
}
int main(void){
 ramfs_file_t file={.capacity=8192,.data_is_pmm=1};
 file.data=pmm_alloc_pages(2);memset(file.data,0,8192);
 vfs_node_t node={.device=&file,.length=8192,.mmap_private_pmm_refs=true};
 views[0]=(struct vma){0x100000,0x102000,MAP_SHARED,3,&node};
 views[1]=(struct vma){0x200000,0x202000,MAP_PRIVATE,3,&node};
 views[2]=(struct vma){0x300000,0x301000,MAP_PRIVATE,3,&node};
 assert(!vma_has_file_page_refs(&views[0]));
 assert(vma_has_file_page_refs(&views[1]) && vma_has_file_page_refs(&views[2]));
 vfs_node_t device={0};
 struct vma driver={.flags=MAP_PRIVATE,.file_node=&device};
 assert(!vma_has_file_page_refs(&driver));
 driver.flags=MAP_PAGECACHE;assert(vma_has_file_page_refs(&driver));
 assert(!vma_has_file_page_refs(NULL));
 assert(memfd_mmap(&node,0x100000,8192,3,MAP_SHARED,0)==0x100000);
 assert(memfd_mmap(&node,0x200000,8192,3,MAP_PRIVATE,0)==0x200000);
 assert(memfd_mmap(&node,0x300000,4096,1,MAP_PRIVATE,4096)==0x300000);
 uint64_t *shared=mapping(0x100000),*priv=mapping(0x200000),*ro=mapping(0x300000);
 assert((*shared&PAGE_MASK)==(*priv&PAGE_MASK));
 assert((*shared&PAGE_FLAG_RW) && !(*shared&PAGE_FLAG_COW));
 assert((*priv&PAGE_FLAG_COW) && !(*priv&PAGE_FLAG_RW) && (*priv&PAGE_FLAG_NX));
 assert((*ro&PAGE_MASK)==(uint64_t)(file.data+4096));
 assert(*ref(file.data)==2 && *ref(file.data+4096)==3);
 /* The client maps first; the server publishes class IDs afterwards. */
 file.data[0]=42;file.data[4096]=99;
 assert(((uint8_t *)(uintptr_t)(*priv&PAGE_MASK))[0]==42);
 assert(((uint8_t *)(uintptr_t)(*ro&PAGE_MASK))[0]==99);
 private_write(0x200000,7);
 assert((*priv&PAGE_MASK)!=(uint64_t)file.data && (*priv&PAGE_FLAG_RW));
 assert(!(*priv&PAGE_FLAG_COW) && file.data[0]==42);
 assert(((uint8_t *)(uintptr_t)(*priv&PAGE_MASK))[0]==7);
 assert(*ref(file.data)==1); /* file ownership survives private CoW */
 teardown_range_locked(NULL,&current,0x100000,8192);
 assert(*ref(file.data)==1 && *ref(file.data+4096)==3);
 teardown_range_locked(NULL,&current,0x200000,8192);
 teardown_range_locked(NULL,&current,0x300000,4096);
 assert(*ref(file.data)==1 && *ref(file.data+4096)==1);
 int before=map_count;
 assert((int64_t)memfd_mmap(&node,0x400000,4096,1,MAP_PRIVATE,1)==-22);
 assert((int64_t)memfd_mmap(&node,0x400000,0,1,MAP_PRIVATE,0)==-22);
 assert((int64_t)memfd_mmap(&node,0x400000,4096,1,MAP_PRIVATE,UINT32_MAX)==-22);
 assert((int64_t)memfd_mmap(&node,0x400000,0xfffff001,1,MAP_PRIVATE,0)==-22);
 assert(map_count==before);
 assert(memfd_mmap(&node,0x400000,4096,5,MAP_PRIVATE,0)==0x400000);
 assert(!(*mapping(0x400000)&PAGE_FLAG_NX));
 views[3]=(struct vma){0x400000,0x401000,MAP_PRIVATE,3,&node};
 teardown_range_locked(NULL,&current,0x400000,4096);
 assert(memfd_mmap(&node,0x500000,4096,0,MAP_PRIVATE,0)==0x500000);
 assert(!(*mapping(0x500000)&(PAGE_FLAG_USER|PAGE_FLAG_RW)));
 assert(*mapping(0x500000)&PAGE_FLAG_NX);
 views[4]=(struct vma){0x500000,0x501000,MAP_PRIVATE,3,&node};
 teardown_range_locked(NULL,&current,0x500000,4096);
 ramfs_free_file_data(&file);
 for(int i=0;i<frame_count;i++)assert(frames[i].refs==0);
 for(int i=0;i<allocation_count;i++)free(allocations[i]);
 puts("PASS: memfd private views observe later server updates; real CoW isolates writes; offsets, protections, actual munmap reference release and invalid ranges");
}
'''
with tempfile.TemporaryDirectory(prefix='avory-memfd-private-') as tmp:
    p = Path(tmp)
    (p/'test.c').write_text(code)
    subprocess.run(['cc','-Wall','-Wextra','-Werror','-fsanitize=undefined',str(p/'test.c'),'-o',str(p/'test')],check=True)
    subprocess.run([str(p/'test')],check=True)
