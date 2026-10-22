#!/usr/bin/env python3
"""Run the actual loader against bounded VFS and allocator failures."""
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[2]
source=(root/'kernel/linuxkpi/src/firmware.c').read_text()
diag=(root/'kernel/linuxkpi/include/linuxkpi/probe_diag.h').read_text()
source='\n'.join(x for x in source.splitlines() if not x.startswith('#include'))
pre=r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#define GFP_KERNEL 0
#define min(a,b) ((a)<(b)?(a):(b))
#define klogf(...) ((void)0)
struct device {int unused;};
struct firmware {size_t size;const unsigned char *data;void *priv;};
static unsigned long long node_size;
static unsigned available,partial,opened,closed,calls,fail_alloc,live,vm_calls;
static void *asc_vfs_kernel_open(const char *p){assert(!strncmp(p,"/lib/firmware/",14));if(!strcmp(p,"/lib/firmware/missing"))return NULL;opened++;return &node_size;}
static unsigned long long asc_vfs_kernel_size64(void *p){assert(p==&node_size);return node_size;}
static void asc_vfs_kernel_close(void *p){assert(p==&node_size);closed++;}
static unsigned char pattern(unsigned i){return (i*7+3)&255;}
static unsigned asc_vfs_kernel_read(void *p,unsigned off,unsigned count,unsigned char *buf){
 assert(p==&node_size && count<=1024*1024);if(off>=available)return 0;
 unsigned n=min(count,available-off);if(partial)n=min(n,partial);
 for(unsigned i=0;i<n;i++)buf[i]=pattern(off+i);return n;
}
static void *allocate(size_t n){if(++calls==fail_alloc)return NULL;void *p=malloc(n);assert(p);live++;return p;}
static void *kzalloc(size_t n,int flags){(void)flags;void *p=allocate(n);if(p)memset(p,0,n);return p;}
static void *kmalloc(size_t n,int flags){(void)flags;return allocate(n);}
static void *vmalloc(size_t n){vm_calls++;return allocate(n);}
static void kfree(const void *p){if(p){live--;free((void*)p);}}
static void kvfree(const void *p){kfree(p);}
'''
post=r'''
static void reset(unsigned long long size){assert(!live && opened==closed);node_size=size;available=size;partial=0;calls=fail_alloc=vm_calls=0;}
int main(void){
 const struct firmware *fw=(void*)1;reset(38061600);
 assert(!firmware_request_nowarn(&fw,"nvidia/ad106/gsp/gsp-535.113.01.bin",NULL));
 assert(fw && fw->size==38061600 && vm_calls==1 && live==2);
 for(unsigned i=0;i<fw->size;i++)assert(fw->data[i]==pattern(i));release_firmware(fw);assert(!live);
 reset(4096);partial=73;assert(!request_firmware(&fw,"small",NULL) && !vm_calls);release_firmware(fw);
 reset(4096);available=100;assert(request_firmware(&fw,"truncated",NULL)==-EIO && !fw && !live);
 reset(0);assert(request_firmware(&fw,"empty",NULL)==-EINVAL && !fw);
 reset(64ULL*1024*1024+1);assert(request_firmware(&fw,"large",NULL)==-EFBIG && !fw && !calls);
 reset((1ULL<<32)+4096);assert(request_firmware(&fw,"wrapped-size",NULL)==-EFBIG && !fw && !calls);
 for(unsigned n=1;n<=2;n++){reset(38061600);fail_alloc=n;assert(request_firmware(&fw,"oom",NULL)==-ENOMEM && !fw && !live);}
 reset(4096);assert(request_firmware(&fw,"missing",NULL)==-ENOENT && !fw);
 const char *bad[]={"","/etc/passwd","../x","a/../x","a//x","a/./x","a/","a\\b"};
 for(unsigned i=0;i<sizeof(bad)/sizeof(*bad);i++){fw=(void*)1;assert(request_firmware(&fw,bad[i],NULL)==-EINVAL && !fw);}
 assert(request_firmware(NULL,"x",NULL)==-EINVAL);release_firmware(NULL);assert(opened==closed && !live);
 puts("PASS: real 38MB load/content/release, partial reads, truncation, zero/oversize/64-bit bounds, OOM unwind and invalid names");
}
'''
with tempfile.TemporaryDirectory(prefix='nouveau-fw-') as tmp:
 p=Path(tmp)/'test';p.with_suffix('.c').write_text(pre+diag+source+post)
 subprocess.run(['cc','-std=gnu11','-Wall','-Wextra','-Werror','-Wno-misleading-indentation','-fsanitize=address,undefined',str(p.with_suffix('.c')),'-o',str(p)],check=True)
 subprocess.run([str(p)],check=True,timeout=30)
