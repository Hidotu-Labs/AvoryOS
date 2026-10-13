#!/usr/bin/env python3
"""Exercise actual data-run cache publication/completion and latency atomics."""
from pathlib import Path
import os, subprocess, tempfile

def extract(path, signature):
    s=Path(path).read_text(); a=s.index(signature); b=s.index('{',a)+1; d=1
    while d:
        d+=(s[b]=='{')-(s[b]=='}'); b+=1
    return s[a:b]+'\n'
header=Path('kernel/src/drivers/storage/block.h').read_text()
a=header.index('struct storage_latency {'); b=header.index('\nstruct block_device {',a)
code=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <pthread.h>
#define EXT2_CACHE_SIZE 2048
static unsigned locked;
static void spinlock_acquire(void *p){(void)p;assert(!locked);locked=1;}
static void spinlock_release(void *p){(void)p;assert(locked);locked=0;}
static bool oom;
static void *kmalloc(size_t n){return oom?NULL:malloc(n);}
struct block_device {int (*write_sectors)(struct block_device *,uint64_t,uint32_t,const void *);};
typedef struct {unsigned block_size;struct {unsigned s_blocks_count;} sb;struct block_device *dev;unsigned cache_lock;struct {unsigned num,writes_inflight;uint64_t write_seq;unsigned char *data;} cache[EXT2_CACHE_SIZE];} ext2_mount_t;
static ext2_mount_t mount;
static unsigned requests,first_block,blocks;
static bool failed;
static unsigned char image[32*4096];
static int write_run(struct block_device *dev,uint64_t lba,uint32_t count,const void *buf){
 (void)dev;assert(!locked);requests++;assert(lba==(uint64_t)first_block*(mount.block_size/512));assert(count==blocks*(mount.block_size/512));
 for(unsigned i=0;i<blocks;i++){unsigned idx=(first_block+i)%EXT2_CACHE_SIZE;assert(mount.cache[idx].writes_inflight==1);if(!oom){assert(mount.cache[idx].num==first_block+i);assert(!memcmp(mount.cache[idx].data,(const unsigned char *)buf+i*mount.block_size,mount.block_size));}}
 if(failed)return -1;memcpy(image,buf,(size_t)blocks*mount.block_size);return 0;
}
'''+header[a:b]+extract('kernel/src/fs/ext2/ext2_block.c','int ext2_write_blocks(')+r'''
static struct storage_latency latency;
static void *samples(void *arg){(void)arg;for(unsigned i=0;i<10000;i++)storage_latency_record(&latency,8,i%2);return NULL;}
int main(void){
 struct block_device dev={.write_sectors=write_run};mount.dev=&dev;mount.sb.s_blocks_count=10000;
 unsigned char source[32*4096];for(unsigned i=0;i<sizeof(source);i++)source[i]=i%251;
 for(unsigned bs=1024;bs<=4096;bs*=2){
  mount.block_size=bs;first_block=2040;blocks=32; /* crosses cache index wrap */
  for(unsigned i=0;i<EXT2_CACHE_SIZE;i++){free(mount.cache[i].data);memset(&mount.cache[i],0,sizeof(mount.cache[i]));}
  requests=0;assert(!ext2_write_blocks(&mount,first_block,blocks,source));assert(requests==1 && !memcmp(image,source,32*bs));
  for(unsigned i=0;i<blocks;i++){unsigned idx=(first_block+i)%EXT2_CACHE_SIZE;assert(!mount.cache[idx].writes_inflight && mount.cache[idx].write_seq==2);}
  failed=true;assert(ext2_write_blocks(&mount,first_block,blocks,source)==-1);failed=false;
  for(unsigned i=0;i<blocks;i++){unsigned idx=(first_block+i)%EXT2_CACHE_SIZE;assert(!mount.cache[idx].num && !mount.cache[idx].writes_inflight && mount.cache[idx].write_seq==4);}
 }
 requests=0;assert(ext2_write_blocks(&mount,9990,32,source)==-1);assert(ext2_write_blocks(&mount,0,1,source)==-1);assert(ext2_write_blocks(&mount,1,33,source)==-1);assert(!requests);
 for(unsigned i=0;i<EXT2_CACHE_SIZE;i++){free(mount.cache[i].data);mount.cache[i].data=NULL;}
 oom=true;blocks=1;assert(!ext2_write_blocks(&mount,first_block,blocks,source));assert(!mount.cache[first_block%EXT2_CACHE_SIZE].writes_inflight);
 pthread_t t[4];for(unsigned i=0;i<4;i++)assert(!pthread_create(&t[i],NULL,samples,NULL));for(unsigned i=0;i<4;i++)assert(!pthread_join(t[i],NULL));
 struct storage_latency snap;storage_latency_snapshot(&latency,&snap);assert(snap.count==40000 && snap.total_ms==320000 && snap.max_ms==8 && snap.errors==20000 && snap.buckets[4]==40000);
 memset(&latency,0,sizeof(latency));unsigned values[]={0,1,2,4,8,16,32,64};for(unsigned i=0;i<8;i++)storage_latency_record(&latency,values[i],false);for(unsigned i=0;i<8;i++)assert(latency.buckets[i]==1);
 puts("PASS: actual run cache publication, wrap, errors, bounds, allocation failure, concurrent latency counters and bucket boundaries");
}
'''
with tempfile.TemporaryDirectory() as tmp:
    p=Path(tmp);(p/'test.c').write_text(code)
    subprocess.run(['cc','-O1','-g','-fsanitize=address,undefined','-pthread',str(p/'test.c'),'-o',str(p/'test')],check=True)
    subprocess.run([str(p/'test')],check=True,env={**os.environ,'ASAN_OPTIONS':'detect_leaks=0'})
