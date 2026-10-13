#!/usr/bin/env python3
"""Actual NVMe submission accounting and procfs output, with device mocks."""
from pathlib import Path
import os, subprocess, tempfile

def extract(path, signature):
    s=Path(path).read_text();a=s.index(signature);b=s.index('{',a)+1;d=1
    while d:
        d+=(s[b]=='{')-(s[b]=='}');b+=1
    return s[a:b]+'\n'
code=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include "drivers/storage/block.h"
#define NVME_CMD_READ 2
#define NVME_CMD_WRITE 1
#define NVME_CMD_FLUSH 0
#define NVME_RW_FUA (1u<<30)
#define klogf(...) ((void)0)
struct nvme_controller {uint32_t generation;char *name;};
struct nvme_iovq {unsigned dummy;};
struct nvme_namespace {struct block_device blkdev;uint32_t lba_size,nsid;};
typedef struct {uint32_t opcode,nsid,cdw10,cdw11,cdw12;uint64_t prp1,prp2;} nvme_command_t;
static struct nvme_iovq queue;
static bool no_queue,prp_fail,submit_fail;
static unsigned submissions;
static int result;
static uint64_t now;
static struct nvme_iovq *nvme_current_vq(struct nvme_controller *c){(void)c;return no_queue?NULL:&queue;}
static int nvme_alloc_cid(struct nvme_controller *c,struct nvme_iovq *q){(void)c;(void)q;return 0;}
static void nvme_free_cid(struct nvme_iovq *q,uint32_t cid){(void)q;(void)cid;}
static bool nvme_build_prp(struct nvme_iovq *q,uint32_t cid,uint64_t va,uint32_t bytes,uint64_t *p1,uint64_t *p2){(void)q;(void)cid;(void)va;(void)bytes;*p1=*p2=0;return !prp_fail;}
static int nvme_sq_submit(struct nvme_controller *c,struct nvme_iovq *q,uint16_t cid,nvme_command_t *cmd,uint32_t generation){(void)c;(void)q;(void)cid;(void)cmd;(void)generation;if(submit_fail)return -1;submissions++;return 0;}
static int nvme_io_wait(struct nvme_controller *c,struct nvme_iovq *q,uint32_t cid,uint32_t generation){(void)c;(void)q;(void)cid;(void)generation;now+=12;return result;}
static uint64_t lapic_timer_get_ms(void){return now;}
static void *kmalloc(size_t n){return malloc(n);}
static void kfree(void *p){free(p);}
static int scnprintf(char *b,size_t n,const char *fmt,...){va_list a;va_start(a,fmt);int z=vsnprintf(b,n,fmt,a);va_end(a);return z<0?0:(size_t)z>=n?(n?n-1:0):z;}
typedef struct {uint32_t length;} vfs_node_t;
static struct nvme_namespace ns;
static int block_count(void){return 1;}
struct block_device *block_get(int i){return i==0?&ns.blkdev:NULL;}
static bool ext3_perf_snapshot(unsigned i,struct block_device **dev,struct storage_latency *wait,struct storage_latency *hold){if(i)return false;*dev=&ns.blkdev;memset(wait,0,sizeof(*wait));memset(hold,0,sizeof(*hold));storage_latency_record(wait,4,false);storage_latency_record(hold,8,true);return true;}
'''
# block_count is declared externally by the real header.
code=code.replace('static int block_count(void)', 'int block_count(void)')
for path, name in [('kernel/src/drivers/storage/nvme.c','static int nvme_io_direct_once('),('kernel/src/drivers/storage/nvme.c','static int nvme_flush_once('),('kernel/src/fs/procfs.c','static int procfs_latency_line('),('kernel/src/fs/procfs.c','static uint32_t procfs_storage_perf_read(')]:
    code+=extract(path,name)
code+=r'''
int main(void){
 struct nvme_controller c={.generation=1};ns.lba_size=4096;ns.nsid=1;strcpy(ns.blkdev.name,"nvme0n1");
 assert(!nvme_io_direct_once(&c,&ns,0,32,0,false,false));assert(ns.blkdev.hw_reads==1 && ns.blkdev.hw_read_bytes==131072);
 result=-1;assert(nvme_io_direct_once(&c,&ns,0,2,0,true,true)==-1);assert(ns.blkdev.hw_writes==1 && ns.blkdev.hw_write_bytes==8192 && ns.blkdev.hw_errors==1);
 submit_fail=true;assert(nvme_io_direct_once(&c,&ns,0,2,0,true,false)==-2);submit_fail=false;assert(ns.blkdev.hw_writes==1);
 prp_fail=true;assert(nvme_io_direct_once(&c,&ns,0,2,0,true,false)==-1);prp_fail=false;
 result=0;assert(!nvme_flush_once(&c,&ns));result=-2;assert(nvme_flush_once(&c,&ns)==-2);result=0;assert(!nvme_flush_once(&c,&ns));
 assert(ns.blkdev.hw_flush.count==3 && ns.blkdev.hw_flush.total_ms==36 && ns.blkdev.hw_flush.max_ms==12 && ns.blkdev.hw_flush.errors==1 && ns.blkdev.hw_flush.buckets[4]==3);
 submit_fail=true;assert(nvme_flush_once(&c,&ns)==-2 && ns.blkdev.hw_flush.count==3);submit_fail=false;
 block_stat_write(&ns.blkdev,32);assert(ns.blkdev.stat_writes==1 && ns.blkdev.stat_write_sectors==32);
 vfs_node_t node={0};unsigned char buf[8192]={0},part[8192]={0};unsigned n=procfs_storage_perf_read(&node,0,sizeof(buf)-1,buf);
 assert(n && node.length==n && strstr((char *)buf,"hw_reads=1 hw_read_bytes=131072 hw_writes=1 hw_write_bytes=8192 hw_errors=1"));
 assert(strstr((char *)buf,"nvme_flush nvme0n1 count=3 total_ms=36 max_ms=12 errors=1"));assert(strstr((char *)buf,"journal_wait nvme0n1 count=1 total_ms=4"));
 for(unsigned off=0;off<n;off+=7){unsigned got=procfs_storage_perf_read(&node,off,7,part+off);assert(got==(n-off<7?n-off:7));}
 assert(!memcmp(buf,part,n));assert(!procfs_storage_perf_read(&node,n,100,part));assert(!procfs_storage_perf_read(&node,UINT32_MAX,UINT32_MAX,part));
 puts("PASS: actual NVMe command/byte/error accounting, flush samples including retries, rejected submissions excluded, procfs formatting and partial reads");
}
'''
with tempfile.TemporaryDirectory() as tmp:
    p=Path(tmp);(p/'test.c').write_text(code)
    subprocess.run(['cc','-O1','-g','-fsanitize=address,undefined','-I','kernel/src',str(p/'test.c'),'-o',str(p/'test')],check=True)
    subprocess.run([str(p/'test')],check=True,env={**os.environ,'ASAN_OPTIONS':'detect_leaks=0'})
