#!/usr/bin/env python3
"""Exercise actual AHCI PRDT/routing code with fragmented heap buffers."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
source = (ROOT/'kernel/src/drivers/storage/ahci.c').read_text()
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
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include "drivers/storage/ahci.h"
#define PAGE_SIZE 4096u
#define VA 0xffff800001000000ULL
static bool contiguous, user;
static uint64_t physical_base = 0x100000;
static uint64_t unmapped = UINT64_MAX;
static uint64_t vmm_virt_to_phys(uint64_t *cr3, uint64_t va) {
 (void)cr3; assert(va >= VA);
 uint64_t offset = va - VA;
 if (offset / PAGE_SIZE == unmapped) return 0;
 return physical_base + (offset / PAGE_SIZE) * (contiguous ? PAGE_SIZE : 8192) + offset % PAGE_SIZE;
}
struct ahci_drive {ahci_port_t *port; uint64_t total_sectors; bool failed;};
static struct ahci_drive ahci_drives[32];
static unsigned direct, bounce;
static bool is_user_ptr(uint64_t ptr) {(void)ptr;return user;}
#define klogf(...) ((void)0)
static int ahci_io_direct(struct ahci_drive *d,uint64_t l,uint32_t c,void *b,int w) {
 (void)d;(void)l;(void)c;(void)b;(void)w;direct++;return 0;
}
static int ahci_io_bounce(struct ahci_drive *d,uint64_t l,uint32_t c,void *b,int w) {
 (void)d;(void)l;(void)c;(void)b;(void)w;bounce++;return 0;
}
'''
code += function('static uint32_t ahci_build_prdt(')
code += function('static int ahci_io(ahci_port_t *port,')
code += r'''
static uint32_t check(uint64_t va, uint32_t bytes) {
 ahci_command_table_t table={0}; uint32_t n=0;
 uint32_t covered=ahci_build_prdt(&table,0,va,bytes,&n), sum=0;
 assert(covered && !(covered & 511) && covered<=bytes && n<=8);
 for(unsigned i=0;i<n;i++) {
  ahci_prdt_entry_t *e=&table.prdt_entry[i];
  uint64_t phys=((uint64_t)e->dbau<<32)|e->dba;
  uint32_t length=e->dbc+1;
  assert(!(phys&1) && !(length&1) && length<=4*1024*1024);
  assert((phys>>32)==((phys+length-1)>>32));
  assert(phys==vmm_virt_to_phys(NULL,va+sum));
  assert(e->i==(i==n-1));sum+=length;
 }
 assert(sum==covered);return covered;
}
int main(void) {
 ahci_port_t port={0}; ahci_drives[0]=(struct ahci_drive){.port=&port,.total_sectors=100000};
 for(unsigned offset=0;offset<4096;offset+=2) {
  assert(check(VA+offset,4096)==4096);
  uint32_t remaining=256*1024, done=0;
  while(remaining) {uint32_t c=check(VA+offset+done,remaining);done+=c;remaining-=c;}
  direct=bounce=0;
  assert(!ahci_io(&port,16,8,(void *)(VA+offset),0)); assert(direct==1 && bounce==0);
 }
 for(unsigned i=0;i<10000;i++) assert(!ahci_io(&port,16,8,(void *)(VA+32),1));
 direct=bounce=0; assert(!ahci_io(&port,16,8,(void *)(VA+1),0) && bounce==1 && !direct);
 user=true;direct=bounce=0;assert(!ahci_io(&port,16,8,(void *)(VA+32),0) && bounce==1 && !direct);user=false;
 contiguous=true;physical_base=0xfffff000;check(VA+32,4*1024*1024);
 contiguous=false;unmapped=1;ahci_command_table_t table={0};uint32_t n=0;
 assert(!ahci_build_prdt(&table,0,VA+32,8192,&n));
 assert(ahci_io(&port,99999,2,(void *)VA,0)==-1);
}
'''
with tempfile.TemporaryDirectory(prefix='ahci-metadata-') as directory:
    path = Path(directory)
    (path/'test.c').write_text(code)
    subprocess.run(['cc','-O2','-Wall','-Wextra','-Werror','-fsanitize=undefined',
                    '-I',str(ROOT/'kernel/src'),str(path/'test.c'),'-o',str(path/'test')],check=True)
    subprocess.run([str(path/'test')],check=True)
print('PASS: 2048 word offsets, fragmented PRDT continuation, 4 GiB boundary, missing mapping, metadata DMA and bounce fallbacks')
