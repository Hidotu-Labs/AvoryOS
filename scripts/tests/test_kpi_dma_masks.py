#!/usr/bin/env python3
"""Execute identity DMA mapping on high RAM, masks, SG and vmalloc inputs."""
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[2]
s=(root/'kernel/linuxkpi/src/dma-mapping.c').read_text()
def fn(sig):
 a=s.index(sig);b=s.index('{',a);end=b+1;depth=1
 while depth:
  depth+=(s[end]=='{')-(s[end]=='}');end+=1
 return s[a:end]+'\n'
code=r'''
#include <assert.h>
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdio.h>
#include <errno.h>
typedef uint64_t dma_addr_t;typedef uint64_t u64;typedef uint64_t phys_addr_t;
#define gpu_diag_resource_enabled() 0
#define gpu_diag_record(...) ((void)0)
#define DMA_MAPPING_ERROR UINT64_MAX
#define DMA_BIT_MASK(n) UINT64_MAX
#define __pa(p) ((uintptr_t)(p)-0x100000000000ULL)
struct device {u64 dma_mask,coherent_dma_mask;};
struct page {uint64_t phys;};
static uint64_t page_to_phys(struct page*p){return p->phys;}
static bool is_vmalloc_addr(const void*p){return (uintptr_t)p>=0x200000000000ULL;}
enum dma_data_direction {DMA_BIDIRECTIONAL};
struct scatterlist {struct page *page;unsigned offset,length;dma_addr_t dma_address;unsigned dma_length;};
struct sg_table {struct scatterlist *sgl;unsigned orig_nents,nents;};
#define sg_page(s) ((s)->page)
#define for_each_sg(head,sg,n,i) for((i)=0,(sg)=(head);(i)<(n);(i)++,(sg)++)
'''
code+='\n'.join(fn(sig) for sig in ['static bool kpi_dma_range_ok(', 'static dma_addr_t kpi_dma_phys(', 'dma_addr_t dma_map_page_attrs(', 'dma_addr_t dma_map_single_attrs(', 'dma_addr_t dma_map_resource(', 'int dma_map_sg(', 'int dma_map_sgtable(', 'int dma_set_mask(', 'int dma_set_coherent_mask('])
code+=r'''
int main(void){
 struct device dev={UINT32_MAX,UINT32_MAX};struct page low={0xfffff000ULL},high={0x100000000ULL};
 assert(dma_map_page_attrs(&dev,&low,0,4096,0,0)==low.phys);
 assert(dma_map_page_attrs(&dev,&low,0,4097,0,0)==DMA_MAPPING_ERROR);
 assert(dma_map_page_attrs(&dev,&high,0,4096,0,0)==DMA_MAPPING_ERROR);
 assert(dma_map_resource(&dev,0x100000000ULL,1,0,0)==DMA_MAPPING_ERROR);
 assert(dma_set_coherent_mask(&dev,0xffffff)==-EIO&&dev.coherent_dma_mask==UINT32_MAX);
 assert(!dma_set_mask(&dev,UINT64_MAX));
 assert(dma_map_page_attrs(&dev,&high,0,4096,0,0)==high.phys);
 assert(dma_map_single_attrs(&dev,(void*)(0x100000000000ULL+high.phys),4096,0,0)==high.phys);
 assert(dma_map_single_attrs(&dev,(void*)0x200000000000ULL,8192,0,0)==DMA_MAPPING_ERROR);
 high.phys=UINT64_MAX-1;assert(dma_map_page_attrs(&dev,&high,4,1,0,0)==DMA_MAPPING_ERROR);high.phys=0x100000000ULL;
 struct scatterlist sg[2]={{.page=&low,.length=4096},{.page=&high,.offset=8,.length=16}};
 struct sg_table table={sg,2,99};
 assert(!dma_map_sgtable(&dev,&table,0,0)&&table.nents==2);
 assert(sg[1].dma_address==high.phys+8&&sg[1].dma_length==16);
 dev.dma_mask=UINT32_MAX;assert(dma_map_sgtable(&dev,&table,0,0)==-EIO&&table.nents==0);
 assert(!sg[0].dma_length&&!sg[1].dma_length);
 puts("PASS: high RAM, range overflow, mask rejection, vmalloc rejection, SG address/length population and failure rollback");
}
'''
with tempfile.TemporaryDirectory(prefix='kpi-dma-mask-') as tmp:
 p=Path(tmp);(p/'test.c').write_text(code)
 subprocess.run(['cc','-std=gnu11','-O2','-Wall','-Wextra','-Werror',str(p/'test.c'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True,timeout=10)
