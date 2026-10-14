#!/usr/bin/env python3
"""Exercise real LinuxKPI page release and DRM/TTM shmem ownership."""
from pathlib import Path
import subprocess
import tempfile
ROOT = Path(__file__).resolve().parents[2]
pages = (ROOT/'kernel/linuxkpi/src/page.c').read_text()
shmem = (ROOT/'kernel/linuxkpi/src/shmem.c').read_text()
page_code = pages[pages.index('/* The final reference'):pages.index('\nvoid put_pages_list')]
shmem_code = shmem[shmem.index('static DEFINE_MUTEX(shmem_page_lock)'):shmem.index('\nstatic int shmem_file_release')]
pre = r'''
#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <sched.h>
#define PAGE_SHIFT 12
#define PAGE_SIZE 4096
#define gpu_diag_resource_enabled() 0
#define gpu_diag_record(...) ((void)0)
#define GFP_KERNEL 0
#define __GFP_ZERO 1
#define XA_PRESENT 0
#define ERR_PTR(n) ((void *)(intptr_t)(n))
#define ERR_CAST(p) ((void *)(p))
#define IS_ERR(p) ((uintptr_t)(p) > (uintptr_t)-4096)
#define DEFINE_MUTEX(name) pthread_mutex_t name = PTHREAD_MUTEX_INITIALIZER
#define mutex_lock(p) pthread_mutex_lock(p)
#define mutex_unlock(p) pthread_mutex_unlock(p)
#define atomic_long_read(p) (*(p))
#define atomic_long_inc(p) (++*(p))
#define atomic_long_sub(n,p) (*(p)-=(n))
#define atomic_long_set(p,n) (*(p)=(n))
#define atomic_dec_and_test(p) (__atomic_sub_fetch((p),1,__ATOMIC_SEQ_CST)==0)
typedef unsigned long pgoff_t;
typedef unsigned gfp_t;
struct page { unsigned long pfn; int _refcount; void *mapping; unsigned long private,index; unsigned compound_order,alloc_order; struct page *head; bool released; unsigned char data[PAGE_SIZE]; };
struct folio { struct page page; };
struct address_space { struct { struct page *slots[8]; } i_pages; long nrpages; };
struct inode { struct address_space *i_mapping; };
static int allocated, freed, insert_error, alloc_error;
static struct page *allocated_pages[2048];
static struct page *compound_head(struct page *p) { return p->head ? p->head : p; }
static unsigned compound_order(struct page *p) { return p->compound_order; }
#define __ClearPageHead(p) ((void)(p))
static struct folio *page_folio(struct page *p) { return (void *)p; }
static void get_page(struct page *p) { p=compound_head(p); assert(!p->released && p->_refcount>0); __atomic_add_fetch(&p->_refcount,1,__ATOMIC_SEQ_CST); }
static struct page *alloc_pages(gfp_t flags,unsigned order) {
 (void)flags; if(alloc_error)return NULL;
 struct page *p=calloc(1,sizeof(*p)); assert(p && allocated<2048);
 p->pfn=allocated+1; p->_refcount=1; p->compound_order=p->alloc_order=order;
 allocated_pages[allocated++]=p; sched_yield(); return p;
}
static void asc_pmm_free_pages(void *phys,size_t count) {
 struct page *p=allocated_pages[((uintptr_t)phys>>PAGE_SHIFT)-1];
 assert(!p->released && p->_refcount==0);
 assert(count==(1UL<<p->alloc_order)); p->released=true; freed++;
}
static struct page *xa_load(void *array,unsigned long index) { assert(index<8); return ((typeof(((struct address_space*)0)->i_pages)*)array)->slots[index]; }
static int xa_insert(void *array,unsigned long index,struct page *page,int flags) {
 (void)flags; if(insert_error)return insert_error;
 if(xa_load(array,index))return -EBUSY;
 ((typeof(((struct address_space*)0)->i_pages)*)array)->slots[index]=page; return 0;
}
static struct page *xa_find(void *array,unsigned long *index,unsigned long end,int flags) {
 (void)flags; for(;*index<8 && *index<=end;(*index)++) { struct page *p=xa_load(array,*index); if(p)return p; } return NULL;
}
static struct page *xa_erase(void *array,unsigned long index) {
 struct page *p=xa_load(array,index); ((typeof(((struct address_space*)0)->i_pages)*)array)->slots[index]=NULL; return p;
}
'''
post = r'''
static struct address_space shared;
static struct page *results[16];
static void *reader(void *arg) { unsigned n=(uintptr_t)arg; results[n]=shmem_read_mapping_page_gfp(&shared,0,0); return NULL; }
int main(void) {
 struct page *p=alloc_pages(0,0); get_page(p); put_page(p); assert(p->_refcount==1 && !p->released);
 put_page(p); assert(p->released && p->_refcount==0 && freed==1);
 p=alloc_pages(0,3); struct page tail={.head=p}; get_page(&tail); put_page(&tail); assert(p->_refcount==1);
 put_page(&tail); assert(p->released && p->_refcount==0);
 p=alloc_pages(0,0); __free_pages(p,0); assert(p->released);
 int before=allocated;
 pthread_t readers[16];
 for(unsigned i=0;i<16;i++)assert(!pthread_create(&readers[i],NULL,reader,(void *)(uintptr_t)i));
 for(unsigned i=0;i<16;i++)pthread_join(readers[i],NULL);
 assert(allocated==before+1 && shared.nrpages==1);
 p=results[0]; assert(p->_refcount==17 && p->mapping==&shared);
 memset(p->data,0xa5,PAGE_SIZE);
 for(unsigned i=0;i<16;i++) { assert(results[i]==p && !p->released && p->data[300]==0xa5); put_page(results[i]); }
 assert(p->_refcount==1 && !p->released);
 /* Simulate TTM swapout (write + put), swapin (read + put), then file release. */
 for(int round=0;round<500;round++) {
  p=shmem_read_mapping_page_gfp(&shared,1,0); assert(!IS_ERR(p) && p->_refcount==2);
  p->data[37]=round; put_page(p); assert(!p->released && p->_refcount==1);
  struct page *q=shmem_read_mapping_page_gfp(&shared,1,0);
  assert(q==p && q->data[37]==(unsigned char)round); put_page(q);
  assert(invalidate_mapping_pages(&shared,1,1)==1 && p->released);
 }
 /* Removing the cache owner must retain an outstanding reader's storage. */
 p=shmem_read_mapping_page_gfp(&shared,0,0); assert(p->_refcount==2);
 assert(invalidate_mapping_pages(&shared,0,0)==1 && !p->released && p->_refcount==1);
 assert(p->data[300]==0xa5 && !p->mapping); put_page(p); assert(p->released && !shared.nrpages);
 insert_error=-ENOMEM; int f=freed;
 assert(IS_ERR(shmem_read_mapping_page_gfp(&shared,2,0))); assert(freed==f+1 && !shared.nrpages);
 insert_error=0; alloc_error=1; assert(IS_ERR(shmem_read_mapping_page_gfp(&shared,2,0))); alloc_error=0;
 assert(IS_ERR(shmem_read_mapping_page_gfp(NULL,2,0)));
 assert(allocated==freed);
 for(int i=0;i<allocated;i++)free(allocated_pages[i]);
 puts("PASS: final put/free, compound refs, 16 concurrent shmem readers, 500 TTM-style swap cycles, truncate with live readers, error cleanup; all pages reclaimed");
}
'''
with tempfile.TemporaryDirectory(prefix='avory-shmem-lifetime-') as tmp:
 p=Path(tmp); src=p/'test.c'; exe=p/'test'
 src.write_text(pre+page_code+shmem_code+post)
 subprocess.run(['cc','-std=gnu11','-O2','-Wall','-Wextra','-Werror','-pthread',str(src),'-o',str(exe)],check=True)
 subprocess.run([str(exe)],check=True)
 # Recreate each original defect separately as negative controls.
 for name,page_variant,shmem_variant in [
  ('double-put',page_code.replace('page_release_block(head, compound_order(head));','__free_pages(head, compound_order(head));'),shmem_code),
  ('borrowed-read',page_code,shmem_code.replace('    get_page(page);','').replace('  get_page(page);',''))]:
  src.write_text(pre+page_variant+shmem_variant+post)
  subprocess.run(['cc','-std=gnu11','-O2','-pthread',str(src),'-o',str(exe)],check=True)
  result=subprocess.run([str(exe)],capture_output=True)
  assert result.returncode!=0, name+' unexpectedly passed'
 print('PASS: original double-decrement and missing caller-reference controls fail')
