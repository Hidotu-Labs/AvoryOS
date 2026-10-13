#!/usr/bin/env python3
"""Real disk preallocation code with in-memory storage and actual leaf mappings."""
from pathlib import Path
import os
import subprocess
import tempfile


def extract(path, signature):
    s = Path(path).read_text()
    start = s.index(signature)
    end = s.index('{', start) + 1
    depth = 1
    while depth:
        depth += (s[end] == '{') - (s[end] == '}')
        end += 1
    return s[start:end] + '\n'


code = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define FS_TYPE_MASK 0xff
#define FS_FILE 1
#define EXT4_EXT_MAGIC 0xf30a
#define EXT4_EXTENTS_FL 0x80000
#define EXT2_FALLOCATE_BATCH_BLOCKS 32
#define EXT4_FALLOCATE_BATCH_BLOCKS 256
#define BS 1024
#define NB 20000
typedef struct {uint16_t eh_magic,eh_entries,eh_max,eh_depth;uint32_t eh_generation;} ext4_extent_header_t;
typedef struct {uint32_t ee_block;uint16_t ee_len,ee_start_hi;uint32_t ee_start_lo;} ext4_extent_t;
typedef struct {uint32_t ei_block,ei_leaf_lo;uint16_t ei_leaf_hi,ei_unused;} ext4_extent_idx_t;
typedef struct {uint32_t i_flags,i_block[15],i_blocks,i_size,i_ctime;} ext2_inode_t;
typedef struct {unsigned block_size;struct {const char *phase;} journal;} ext2_mount_t;
typedef struct {void *device;unsigned inode,length,flags,ctime;} vfs_node_t;
static ext2_mount_t mount={.block_size=BS};
static ext2_inode_t image;
static unsigned char disk[NB][BS];
static unsigned indirect[NB],next_block,depth,starts,writes,allocs,frees,max_batch,batch_allocs,inserts,data_requests;
static unsigned fail_start,fail_stop,fail_inode,fail_write,fail_alloc,fail_map,compete;
static unsigned run_limit,fail_data_request;
static void *kmalloc(size_t z){return malloc(z);}
static void kfree(void *p){free(p);}
static uint32_t ext2_current_time(void){return 42;}
static int ext3_journal_start(ext2_mount_t *m){(void)m;assert(!depth);starts++;if(starts==fail_start)return -1;depth=1;batch_allocs=0;if(starts==compete)image.i_size=1000*BS;return 0;}
static int ext3_journal_stop(ext2_mount_t *m){(void)m;assert(depth);depth=0;if(batch_allocs>max_batch)max_batch=batch_allocs;return fail_stop?-1:0;}
static int ext2_read_inode(ext2_mount_t *m,unsigned ino,ext2_inode_t *out){(void)m;(void)ino;assert(depth);if(fail_inode)return -1;*out=image;return 0;}
static int ext2_write_inode(ext2_mount_t *m,unsigned ino,const ext2_inode_t *in){(void)m;(void)ino;assert(depth);image=*in;return 0;}
static int ext2_read_block(ext2_mount_t *m,unsigned b,void *out){(void)m;assert(b<NB);memcpy(out,disk[b],BS);return 0;}
static int ext2_write_block(ext2_mount_t *m,unsigned b,const void *in){(void)m;assert(depth && b<NB);if(fail_write)return -1;memcpy(disk[b],in,BS);writes++;return 0;}
static uint32_t ext2_alloc_block(ext2_mount_t *m){(void)m;assert(depth);if(fail_alloc && allocs>=fail_alloc)return 0;assert(next_block<NB);allocs++;batch_allocs++;return next_block++;}
static int ext2_free_block(ext2_mount_t *m,unsigned b){(void)m;(void)b;assert(depth);frees++;return 0;}
static int ext2_get_block_num_checked(ext2_mount_t *m,ext2_inode_t *in,unsigned logical,unsigned *out){(void)m;(void)in;assert(depth && logical<NB);if(fail_map)return -1;*out=indirect[logical];return 0;}
static int ext2_set_block_num(ext2_mount_t *m,ext2_inode_t *in,unsigned logical,unsigned b){(void)m;(void)in;assert(depth && logical<NB);indirect[logical]=b;return 0;}
'''
path = 'kernel/src/fs/ext4/ext4_extent.c'
for sig in ['uint64_t ext4_extent_get_pblock(', 'bool ext4_inode_has_extents(',
            'uint32_t ext4_get_extent_run(', 'uint32_t ext4_get_block_num(',
            'void ext4_extent_init_inode(', 'static void ext4_extent_set_pblock(',
            'static int ext4_extent_insert_leaf_entry(']:
    code += extract(path, sig)
code += r'''
static __attribute__((unused)) uint32_t ext2_alloc_run_hint(ext2_mount_t *m,unsigned goal,unsigned wanted,unsigned *count){
 (void)goal;unsigned first=0;*count=0;
 if(run_limit && wanted>run_limit)wanted=run_limit;
 while(*count<wanted){unsigned b=ext2_alloc_block(m);if(!b)break;if(!first)first=b;(*count)++;}
 return first;
}
static __attribute__((unused)) int ext2_write_blocks(ext2_mount_t *m,unsigned first,unsigned count,const void *in){
 assert(count && count<=32);data_requests++;
 if(data_requests==fail_data_request)return -1;
 for(unsigned i=0;i<count;i++)if(ext2_write_block(m,first+i,(const unsigned char *)in+i*BS))return -1;
 return 0;
}
static int ext4_extent_insert(ext2_mount_t *m,ext2_inode_t *in,unsigned ino,unsigned logical,uint64_t phys,uint16_t z){
 (void)m;(void)ino;inserts++;ext4_extent_header_t *h=(void *)in->i_block;assert(!h->eh_depth);
 /* The new block must be zero before it becomes readable through a map. */
 for(unsigned b=0;b<z;b++)for(unsigned i=0;i<BS;i++)assert(!disk[phys+b][i]);
 return ext4_extent_insert_leaf_entry(h,(void *)(h+1),logical,phys,z);
}
'''
code += extract(os.environ.get("FALLOCATE_SOURCE", "kernel/src/fs/ext2/ext2_fallocate.c"), 'int ext2_fallocate_impl(')
code += r'''
static void reset(bool extents){memset(&image,0,sizeof(image));memset(disk,0xa5,sizeof(disk));memset(indirect,0,sizeof(indirect));next_block=10;depth=starts=writes=allocs=frees=max_batch=batch_allocs=0;fail_start=fail_stop=fail_inode=fail_write=fail_alloc=fail_map=compete=inserts=data_requests=run_limit=fail_data_request=0;if(extents)ext4_extent_init_inode(&image);}
static unsigned mapping(unsigned l){return ext4_inode_has_extents(&image)?ext4_get_block_num(&mount,&image,l):indirect[l];}
int main(void){
 vfs_node_t n={.device=&mount,.inode=1,.flags=FS_FILE};
 for(unsigned extents=0;extents<=1;extents++){
  unsigned batch=extents?EXT4_FALLOCATE_BATCH_BLOCKS:EXT2_FALLOCATE_BATCH_BLOCKS;
  reset(extents);n.length=0;
  /* Exact allocation from the reported SDDM greeter syscall. */
  assert(!ext2_fallocate_impl(&n,0,0,0x1018068));
  assert(!depth && n.length==0x1018068 && image.i_size==n.length);
  assert(allocs==(0x1018068+BS-1)/BS && max_batch==batch);
  for(unsigned l=0;l<allocs;l++){unsigned b=mapping(l);assert(b);for(unsigned j=0;j<BS;j++)assert(!disk[b][j]);}
  if(extents){
   assert(inserts==(allocs+batch-1)/batch && starts==inserts);
   assert(data_requests==(allocs+31)/32);
   printf("Ext4 16 MiB reservation: %u transactions / extent inserts, %u bounded data requests (previously %u transactions)\n",starts,data_requests,(allocs+31)/32);
  }
  unsigned old_allocs=allocs,old_starts=starts,old_requests=data_requests;
  unsigned b=mapping(0);memset(disk[b],0x5a,BS);
  assert(!ext2_fallocate_impl(&n,0,0,0x1018068) && allocs==old_allocs);
  if(extents)assert(starts-old_starts==(allocs+batch-1)/batch && data_requests==old_requests);
  for(unsigned j=0;j<BS;j++)assert(disk[b][j]==0x5a);
  reset(extents);image.i_size=n.length=17;
  assert(!ext2_fallocate_impl(&n,1,BS+7,40*BS));
  assert(image.i_size==17 && n.length==17 && !mapping(0) && mapping(1) && mapping(41));
  assert(image.i_blocks==41*(BS/512));
  /* Reopening uses the persistent inode size, including KEEP_SIZE. */
  vfs_node_t reopened=n;reopened.length=image.i_size;assert(reopened.length==17);
  compete=starts+2;
  assert(!ext2_fallocate_impl(&n,0,0,(batch+38)*BS));assert(image.i_size==1000*BS && n.length==1000*BS);
  reset(extents);image.i_size=n.length=50*BS;
  assert(!ext2_fallocate_impl(&n,1,14*BS,BS));
  b=mapping(14);memset(disk[b],0x5a,BS);
  assert(!ext2_fallocate_impl(&n,0,0,40*BS));
  assert(mapping(14)==b && image.i_size==50*BS);
  for(unsigned j=0;j<BS;j++)assert(disk[b][j]==0x5a);
  for(unsigned l=0;l<40;l++)assert(mapping(l));
  if(extents){
   /* A larger reservation must stop before an existing sparse extent. */
   reset(true);image.i_size=n.length=700*BS;
   assert(!ext2_fallocate_impl(&n,1,200*BS,BS));
   b=mapping(200);memset(disk[b],0x5a,BS);
   assert(!ext2_fallocate_impl(&n,0,0,600*BS));
   assert(mapping(200)==b && image.i_size==700*BS);
   for(unsigned l=0;l<600;l++)assert(mapping(l));
   for(unsigned j=0;j<BS;j++)assert(disk[b][j]==0x5a);
   reset(true);fail_write=1;
   assert(ext2_fallocate_impl(&n,0,0,40*BS)==-5 && !depth && !mapping(0) && frees==40);
   reset(true);fail_data_request=2;
   assert(ext2_fallocate_impl(&n,0,0,300*BS)==-5 && !depth && !mapping(0) && frees==256 && writes==32);
   reset(true);run_limit=1;
   assert(!ext2_fallocate_impl(&n,0,0,70*BS));
   assert(starts==3 && max_batch==32 && inserts==70);
  }
  reset(extents);n.length=17;image.i_size=17;fail_alloc=3;
  assert(ext2_fallocate_impl(&n,0,0,40*BS)==-28 && !depth);
  assert(image.i_size==17 && n.length==17 && image.i_blocks==6);
  reset(extents);fail_write=1;
  assert(ext2_fallocate_impl(&n,0,0,BS)==-5 && !depth && !mapping(0) && frees==1);
  reset(extents);fail_start=2;
  assert(ext2_fallocate_impl(&n,0,0,(batch+8)*BS)==-5 && !depth && allocs==batch && !image.i_size);
  reset(extents);fail_inode=1;
  assert(ext2_fallocate_impl(&n,0,0,BS)==-5 && !depth && !allocs);
  reset(extents);fail_stop=1;
  assert(ext2_fallocate_impl(&n,0,0,BS)==-5 && !depth);
 }
 reset(false);fail_map=1;assert(ext2_fallocate_impl(&n,0,0,BS)==-5 && !allocs && !depth);
 assert(ext2_fallocate_impl(&n,2,0,BS)==-95);
 assert(ext2_fallocate_impl(&n,0,UINT32_MAX,BS)==-27);
 assert(ext2_fallocate_impl(&n,0,0,0)==-22);
 puts("PASS: SDDM reservation, complete zero readback, existing-data preservation, KEEP_SIZE reopen, bounded concurrent snapshots, ENOSPC and I/O failures (extent and indirect inodes)");
}
'''
with tempfile.TemporaryDirectory(prefix='avory-fallocate-') as tmp:
    src = Path(tmp) / 'test.c'
    exe = Path(tmp) / 'test'
    src.write_text(code)
    subprocess.run(['cc', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', str(src), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True,
                   env={**os.environ, "ASAN_OPTIONS": "detect_leaks=0"})
