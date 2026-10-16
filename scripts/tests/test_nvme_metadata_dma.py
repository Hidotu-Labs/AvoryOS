#!/usr/bin/env python3
"""Actual NVMe routing/PRP code: heap offsets, fragmented pages, fallbacks."""
from pathlib import Path
import subprocess
import sys
import tempfile

source = Path(sys.argv[1] if len(sys.argv) > 1 else 'kernel/src/drivers/storage/nvme.c').read_text()
def function(signature):
    start = source.index(signature)
    end = source.index('{', start) + 1
    depth = 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end] + '\n'

code = r'''
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#define NVME_PAGE_SIZE 4096u
#define NVME_MAX_TRANSFER (4u*1024u*1024u)
#define NVME_PRP_LIST_ENTRIES 512u
#define NVME_PRP_PAGES 3u
#define NVME_MAX_PRP_ENTRIES 1534u
struct nvme_controller {bool started,io_ready,failed;uint32_t max_transfer;};
struct nvme_namespace {struct nvme_controller *ctrl;bool present;uint64_t total_sectors;uint32_t lba_size,lba_shift;};
struct nvme_iovq {struct {uint64_t phys[3];} prp[1];};
static uint64_t lists[3][512];
static struct nvme_iovq queue={.prp={{{0x900000,0x901000,0x902000}}}};
static _Alignas(4096) uint8_t buffer[128*1024];
static unsigned direct,bounce,partial;
static bool user;
static int io_error;
static uint64_t last1,last2;
static uint64_t *nvme_prp_page(struct nvme_iovq *q,uint32_t cid,unsigned page){(void)q;assert(!cid && page<3);return lists[page];}
static uint64_t vmm_virt_to_phys(uint64_t *cr3,uint64_t va){
 (void)cr3;
 assert(va>=(uintptr_t)buffer && va<(uintptr_t)(buffer+sizeof(buffer)));
 uint64_t offset=va-(uintptr_t)buffer;
 // Deliberately noncontiguous physical pages.
 return 0x100000+((offset/4096)*8192)+(offset%4096);
}
static bool is_user_ptr(uint64_t ptr){(void)ptr;return user;}
'''
prp = function('static bool nvme_build_prp(')
# Replace only the privileged CR3 read; keep all production PRP arithmetic.
prp = prp.replace('__asm__ volatile("mov %%cr3, %0" : "=r"(cr3));', 'cr3 = 0;')
code += prp
code += r'''
static int nvme_io_direct(struct nvme_controller *c,struct nvme_namespace *n,uint64_t slba,uint32_t blocks,uint64_t va,bool write,bool fua){
 (void)c;(void)slba;(void)write;(void)fua;direct++;
 assert(nvme_build_prp(&queue,0,va,blocks*n->lba_size,&last1,&last2));
 return io_error;
}
static int nvme_io_bounce(struct nvme_controller *c,struct nvme_namespace *n,uint64_t slba,uint32_t blocks,uint8_t *buf,bool write,bool user_ptr,uint32_t bytes,bool fua){
 (void)c;(void)n;(void)slba;(void)blocks;(void)buf;(void)write;(void)user_ptr;(void)bytes;(void)fua;bounce++;return io_error;
}
static int nvme_io_partial(struct nvme_namespace *n,uint64_t block,uint8_t *buf,uint32_t offset,uint32_t len,bool write,bool user_ptr,bool fua){
 (void)n;(void)block;(void)buf;(void)offset;(void)len;(void)write;(void)user_ptr;(void)fua;partial++;return io_error;
}
'''
code += function('static int nvme_io_range(')
code += function('static int nvme_io(')
code += r'''
int main(void){
 struct nvme_controller ctrl={.started=true,.io_ready=true,.max_transfer=8192};
 struct nvme_namespace ns={.ctrl=&ctrl,.present=true,.total_sectors=4096,.lba_size=512,.lba_shift=9};
 for(unsigned offset=0;offset<4096;offset+=4){
  direct=bounce=0;
  assert(!nvme_io(&ns,16,8,buffer+offset,true,false));
  assert(direct==1 && !bounce);
  assert(last1==0x100000+offset);
  assert(last2==(offset?0x102000:0));
 }
 // A 4KiB heap allocation typically starts +32 bytes into a page.
 direct=bounce=0;
 for(unsigned i=0;i<10000;i++)assert(!nvme_io(&ns,16,8,buffer+32,true,false));
 assert(direct==10000 && bounce==0);
 direct=bounce=0;
 assert(!nvme_io(&ns,16,17,buffer+32,false,false));
 assert(direct==2 && !bounce); // max_transfer splitting with PRP list in first chunk
 assert(lists[0][0]==0x102000 && lists[0][1]==0x104000);
 for(unsigned offset=1;offset<=3;offset++){
  direct=bounce=0;
  assert(!nvme_io(&ns,16,8,buffer+offset,true,false));
  assert(!direct && bounce==1);
  assert(!nvme_build_prp(&queue,0,(uintptr_t)(buffer+offset),4096,&last1,&last2));
 }
 user=true;direct=bounce=0;
 assert(!nvme_io(&ns,16,8,buffer+32,true,false) && bounce==1 && !direct);
 user=false;io_error=-1;
 assert(nvme_io(&ns,16,8,buffer+32,true,false)==-1);
 io_error=-2;assert(nvme_io(&ns,16,8,buffer+32,true,false)==-2);
 io_error=0;ns.lba_size=4096;ns.lba_shift=12;partial=direct=bounce=0;
 assert(!nvme_io(&ns,1,16,buffer+32,true,false));
 assert(partial==2 && direct==1 && !bounce);
 ctrl.failed=true;direct=bounce=0;
 assert(nvme_io(&ns,16,8,buffer+32,true,false)==-1 && !direct && !bounce);
 puts("PASS: 1024 PRP offsets on fragmented pages, 10000 metadata writes without bounce allocations, PRP lists/chunks, user/unaligned fallback, errors and 4Kn edges");
}
'''
with tempfile.TemporaryDirectory(prefix='avory-nvme-dma-') as tmp:
    p = Path(tmp)
    (p/'test.c').write_text(code)
    subprocess.run(['cc','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',str(p/'test.c'),'-o',str(p/'test')],check=True)
    subprocess.run([str(p/'test')],check=True)
