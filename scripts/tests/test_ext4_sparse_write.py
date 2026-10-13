#!/usr/bin/env python3
"""Actual extent lookup/allocation and write code, using an in-memory disk."""
from pathlib import Path
import os
import subprocess
import tempfile

def extract(path, signature):
    s = Path(path).read_text(); start = s.index(signature)
    end = s.index('{', start) + 1; depth = 1
    while depth:
        depth += (s[end] == '{') - (s[end] == '}'); end += 1
    return s[start:end] + '\n'

extent = os.environ.get('EXTENT_SOURCE', 'kernel/src/fs/ext4/ext4_extent.c')
write = os.environ.get('WRITE_SOURCE', 'kernel/src/fs/ext4/ext4_mount.c')
prefix = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#define EXT4_EXT_MAGIC 0xf30a
#define EXT4_EXTENTS_FL 0x80000
#define BS 1024
typedef struct {uint16_t eh_magic,eh_entries,eh_max,eh_depth;uint32_t eh_generation;} ext4_extent_header_t;
typedef struct {uint32_t ee_block;uint16_t ee_len,ee_start_hi;uint32_t ee_start_lo;} ext4_extent_t;
typedef struct {uint32_t ei_block,ei_leaf_lo;uint16_t ei_leaf_hi,ei_unused;} ext4_extent_idx_t;
typedef struct {uint32_t i_flags,i_block[15],i_blocks,i_size,i_mtime,i_ctime;} ext2_inode_t;
typedef struct {unsigned block_size,inodes_per_group;struct {const char *phase;} journal;struct {unsigned s_blocks_per_group,s_first_data_block,s_free_blocks_count;} sb;} ext2_mount_t;
typedef struct {void *device;unsigned inode,length;} vfs_node_t;
static ext2_mount_t mount={.block_size=BS,.inodes_per_group=128};
static ext2_inode_t image;
static unsigned char disk[512][BS];
static unsigned next_block,depth,starts,mutate_on_start,fail_restart,fail_stop,fail_read,fail_write,fail_alloc;
static bool user;
static uintptr_t user_start, user_end;
static unsigned long copy_limit;
static void *kmalloc(size_t n){return malloc(n);}
static void kfree(void *p){free(p);}
static void klog_puts(const char *s){(void)s;}
static void klog_uint64(uint64_t n){(void)n;}
static uint32_t ext2_current_time(void){return 42;}
static bool is_user_ptr(uint64_t p){return user && p>=user_start && p<user_end;}
static unsigned long copy_from_user(void *dst,const void *src,unsigned long n){assert(!depth);unsigned long copied=n<copy_limit?n:copy_limit;memcpy(dst,src,copied);return n-copied;}
static int ext2_read_block(ext2_mount_t *m,uint32_t b,void *out){(void)m;assert(b<512);memcpy(out,disk[b],BS);return 0;}
static int ext2_write_block(ext2_mount_t *m,uint32_t b,const void *in){(void)m;assert(depth && b<512);if(fail_write)return -1;memcpy(disk[b],in,BS);return 0;}
static unsigned data_requests,max_run;
static int ext2_write_blocks(ext2_mount_t *m,uint32_t first,uint32_t count,const void *in){
 assert(depth && count && count<=32);data_requests++;if(count>max_run)max_run=count;
 for(unsigned i=0;i<count;i++)if(ext2_write_block(m,first+i,(const unsigned char *)in+i*BS))return -1;
 return 0;
}
static int ext2_read_inode(ext2_mount_t *m,unsigned ino,ext2_inode_t *out){(void)m;(void)ino;if(fail_read && starts==fail_read)return -1;*out=image;return 0;}
static int ext2_write_inode(ext2_mount_t *m,unsigned ino,const ext2_inode_t *in){(void)m;(void)ino;assert(depth);image=*in;return 0;}
static void add_mapping(ext2_inode_t *in,unsigned logical,unsigned physical,unsigned count);
static int ext3_journal_start(ext2_mount_t *m){(void)m;if(!depth){starts++;if(fail_restart && starts==fail_restart)return -1;if(mutate_on_start==starts)add_mapping(&image,50,450,1);}depth++;return 0;}
static int ext3_journal_stop(ext2_mount_t *m){(void)m;assert(depth);depth--;return !depth && fail_stop?-1:0;}
static uint32_t ext2_alloc_run_hint(ext2_mount_t *m,uint32_t goal,uint32_t wanted,uint32_t *count){(void)m;(void)goal;if(fail_alloc){*count=0;return 0;}*count=wanted;unsigned b=next_block;next_block+=wanted;assert(next_block<400);return b;}
static int ext2_free_block(ext2_mount_t *m,uint32_t b){(void)m;(void)b;return 0;}
static uint32_t ext2_write_impl(vfs_node_t *n,uint32_t o,uint32_t z,uint8_t *b){(void)n;(void)o;(void)z;(void)b;assert(!depth);return 123;}
'''
code = prefix
for name in ['uint64_t ext4_extent_get_pblock(', 'bool ext4_inode_has_extents(',
             'uint32_t ext4_get_extent_run(', 'uint32_t ext4_get_block_num(',
             'void ext4_extent_init_inode(', 'static void ext4_extent_set_pblock(',
             'static int ext4_extent_insert_leaf_entry(']:
    code += extract(extent, name)
code += r'''
static int ext4_extent_insert(ext2_mount_t *m,ext2_inode_t *in,unsigned ino,unsigned logical,uint64_t physical,uint16_t count){
 (void)m;(void)ino;ext4_extent_header_t *h=(void *)in->i_block;assert(h->eh_depth==0);
 return ext4_extent_insert_leaf_entry(h,(void *)(h+1),logical,physical,count);
}
static void add_mapping(ext2_inode_t *in,unsigned logical,unsigned physical,unsigned count){assert(!ext4_extent_insert(&mount,in,1,logical,physical,count));}
'''
code += extract(extent, 'int ext4_alloc_extent(')
code += '#define EXT2_WRITE_BATCH_BLOCKS 32\n'
code += extract('kernel/src/fs/ext2/ext2_file.c', 'uint32_t ext2_write_staged(')
code += extract(write, 'static uint32_t ext4_write_kernel(')
code += extract(write, 'uint32_t ext4_write_impl(')
code += r'''
static void reset(void){memset(&image,0,sizeof(image));memset(disk,0,sizeof(disk));ext4_extent_init_inode(&image);next_block=10;depth=starts=mutate_on_start=fail_restart=fail_stop=fail_read=fail_write=fail_alloc=0;user=false;copy_limit=ULONG_MAX;data_requests=max_run=0;}
int main(int argc,char **argv){
 (void)argc;vfs_node_t n={.device=&mount,.inode=1};unsigned char data[40*BS];memset(data,0x5a,sizeof(data));user_start=(uintptr_t)data;user_end=user_start+sizeof(data);
 if(!strcmp(argv[1],"sparse") || !strcmp(argv[1],"all")){
  reset();add_mapping(&image,14,440,1);memset(disk[440],0xa5,BS);image.i_size=30*BS;
  assert(ext4_write_impl(&n,BS,20*BS,data)==20*BS && !depth);
  assert(ext4_get_block_num(&mount,&image,14)==440 && disk[440][0]==0x5a);
  puts("PASS: sparse write stops reservations at existing logical extents");
 }
 if(!strcmp(argv[1],"snapshot") || !strcmp(argv[1],"all")){
  for(unsigned start=1;start<=2;start++){
   reset();mutate_on_start=start;
   assert(ext4_write_impl(&n,0,40*BS,data)==40*BS && !depth);
   assert(ext4_get_block_num(&mount,&image,50)==450);
  }
  puts("PASS: initial and between-batch inode snapshots preserve a competing writer's mapping");
 }
 if(!strcmp(argv[1],"all")){
  reset();assert(ext4_write_impl(&n,0,40*BS,data)==40*BS && starts==2 && data_requests==2 && max_run==32);
  for(unsigned i=0;i<40;i++)assert(!memcmp(disk[ext4_get_block_num(&mount,&image,i)],data+i*BS,BS));
  reset();user=true;assert(ext4_write_impl(&n,17,40*BS,data)==40*BS && starts==2 && max_run==31);
  assert(disk[ext4_get_block_num(&mount,&image,0)][0]==0);
  assert(disk[ext4_get_block_num(&mount,&image,40)][17]==0);
  reset();add_mapping(&image,0,440,2);add_mapping(&image,2,450,2);image.i_size=4*BS;
  assert(ext4_write_impl(&n,0,4*BS,data)==4*BS && data_requests==2 && max_run==2);
  assert(!memcmp(disk[440],data,BS) && !memcmp(disk[451],data+3*BS,BS));
  puts("PASS: bounded 32-block submissions, complete readback, unaligned zeroing and fragmented runs");
  reset();user=true;mutate_on_start=2;
  assert(ext4_write_impl(&n,17,40*BS,data)==40*BS && starts==2 && !depth);
  assert(ext4_get_block_num(&mount,&image,50)==450);
  reset();user=true;fail_stop=1;assert((int32_t)ext4_write_impl(&n,0,BS,data)==-5 && !depth);
  reset();user=true;copy_limit=0;assert((int32_t)ext4_write_impl(&n,0,BS,data)==-14 && !starts);
  reset();user=true;copy_limit=4;add_mapping(&image,0,440,1);memset(disk[440],0xa5,BS);
  assert(ext4_write_impl(&n,0,BS,data)==4 && disk[440][4]==0xa5 && !depth);
  reset();fail_write=1;assert((int32_t)ext4_write_impl(&n,0,BS,data)==-5 && !depth);
  reset();fail_stop=1;assert((int32_t)ext4_write_impl(&n,0,BS,data)==-5 && !depth);
  reset();fail_alloc=1;assert((int32_t)ext4_write_impl(&n,0,BS,data)==-28 && !depth);
  reset();fail_restart=2;assert((int32_t)ext4_write_impl(&n,0,40*BS,data)==-5 && !depth);
  reset();mutate_on_start=2;fail_read=2;assert((int32_t)ext4_write_impl(&n,0,40*BS,data)==-5 && !depth && ext4_get_block_num(&mount,&image,50)==450);
  puts("PASS: user-copy suffix preservation, data/commit/exhaustion/restart errors and failed snapshot cleanup");
  reset();ext4_extent_header_t *root=(void *)image.i_block;root->eh_depth=1;root->eh_entries=2;
  ext4_extent_idx_t *idx=(void *)(root+1);idx[0]=(ext4_extent_idx_t){.ei_block=0,.ei_leaf_lo=400};idx[1]=(ext4_extent_idx_t){.ei_block=14,.ei_leaf_lo=401};
  ext4_extent_header_t *leaf=(void *)disk[400];*leaf=(ext4_extent_header_t){.eh_magic=EXT4_EXT_MAGIC,.eh_entries=1,.eh_max=4};
  ext4_extent_t *ex=(void *)(leaf+1);*ex=(ext4_extent_t){.ee_block=0,.ee_len=1,.ee_start_lo=440};
  leaf=(void *)disk[401];*leaf=(ext4_extent_header_t){.eh_magic=EXT4_EXT_MAGIC,.eh_entries=1,.eh_max=4};ex=(void *)(leaf+1);*ex=(ext4_extent_t){.ee_block=14,.ee_len=1,.ee_start_lo=441};
  unsigned run=0;assert(!ext4_get_extent_run(&mount,&image,1,&run) && run==13);
  idx[0].ei_block=14;idx[0].ei_leaf_lo=401;root->eh_entries=1;
  assert(!ext4_get_extent_run(&mount,&image,1,&run) && run==13);
  puts("PASS: hole boundaries across index siblings and before first index");
 }
}
'''
code = code.replace('#include <assert.h>', '#include <assert.h>\n#include <limits.h>')
with tempfile.TemporaryDirectory() as tmp:
    p = Path(tmp); (p / 'test.c').write_text(code)
    subprocess.run(['cc', '-O1', '-g', '-fsanitize=address,undefined', str(p / 'test.c'), '-o', str(p / 'test')], check=True)
    subprocess.run([str(p / 'test'), os.environ.get('EXT4_CASE', 'all')], check=True,
                   env={**os.environ, 'ASAN_OPTIONS': 'detect_leaks=0'})
