#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#define BS 1024
#define NB 8192
#define EXT4_EXT_MAGIC 0xf30a
#define EXT4_EXTENTS_FL 0x80000
#define EXT2_FEATURE_COMPAT_HAS_JOURNAL 4
struct block_device {int unused;};
typedef struct {uint16_t eh_magic,eh_entries,eh_max,eh_depth;uint32_t eh_generation;} __attribute__((packed)) ext4_extent_header_t;
typedef struct {uint32_t ee_block;uint16_t ee_len,ee_start_hi;uint32_t ee_start_lo;} __attribute__((packed)) ext4_extent_t;
typedef struct {uint32_t ei_block,ei_leaf_lo;uint16_t ei_leaf_hi,ei_unused;} __attribute__((packed)) ext4_extent_idx_t;
typedef struct {uint32_t i_flags,i_block[15],i_blocks,i_size;} ext2_inode_t;
typedef struct {unsigned block_size,inodes_per_group;uint64_t blocks_count,free_blocks_count;struct {bool active;} journal;struct {unsigned s_blocks_per_group,s_first_data_block;} sb;struct block_device *dev;} ext2_mount_t;
static unsigned char disk[NB][BS];static unsigned char *staged[NB];
static unsigned next_block=1,home_writes,flushes,stage_calls,unique;static bool fail_stage;
static void *kmalloc(size_t n){return malloc(n);}static void *kcalloc(size_t n,size_t s){return calloc(n,s);}static void kfree(void *p){free(p);}
static void klog_puts(const char *s){(void)s;}static void klog_uint64(uint64_t n){(void)n;}
static int ext2_read_block(ext2_mount_t *m,uint64_t b,void *p){(void)m;assert(b<NB);memcpy(p,staged[b]?staged[b]:disk[b],BS);return 0;}
static int ext2_write_block(ext2_mount_t *m,uint64_t b,const void *p){(void)m;assert(b<NB);home_writes++;memcpy(disk[b],p,BS);return 0;}
static int block_flush(struct block_device *d){(void)d;flushes++;return 0;}
static int ext3_journal_block(ext2_mount_t *m,uint64_t b,const void *p){assert(m->journal.active&&b<NB);if(fail_stage)return -5;stage_calls++;if(!staged[b]){staged[b]=malloc(BS);assert(staged[b]);unique++;}memcpy(staged[b],p,BS);return 0;}
static int ext3_journal_start(ext2_mount_t *m){(void)m;return 0;}static int ext3_journal_stop(ext2_mount_t *m){(void)m;return 0;}
static uint64_t ext2_alloc_block_extent(ext2_mount_t *m){(void)m;assert(next_block<NB);return next_block++;}
static uint64_t ext2_alloc_block_extent_hint(ext2_mount_t *m,uint64_t goal){(void)goal;return ext2_alloc_block_extent(m);}
static uint64_t ext2_alloc_run_hint(ext2_mount_t *m,uint64_t goal,uint32_t n,uint32_t *count){(void)goal;*count=n;uint64_t b=next_block;next_block+=n;assert(next_block<NB);(void)m;return b;}
static int ext2_free_block(ext2_mount_t *m,uint64_t b){(void)m;(void)b;return 0;}
static int ext2_write_inode(ext2_mount_t *m,uint32_t ino,ext2_inode_t *in){(void)m;(void)ino;(void)in;return 0;}
uint64_t ext4_get_extent_run(ext2_mount_t *,ext2_inode_t *,uint32_t,uint32_t *);
void ext4_extent_init_inode(ext2_inode_t *);
int ext4_extent_insert(ext2_mount_t *,ext2_inode_t *,uint32_t,uint32_t,uint64_t,uint16_t);
#include EXTENT_SOURCE
int main(void){
 ext2_mount_t m={.block_size=BS,.inodes_per_group=100,.blocks_count=1000000,.free_blocks_count=NB-1,.journal={.active=true}};
 ext2_inode_t inode={0};ext4_extent_init_inode(&inode);
 /* Force leaf and index splits, re-reading modified nodes throughout the same
    transaction while every home block still contains its old image. */
 for(unsigned i=0;i<1600;i++){assert(!ext4_extent_insert(&m,&inode,1,i*3,100000+i*7,1));for(unsigned j=0;j<=i;j++)assert(ext4_get_block_num(&m,&inode,j*3)==100000+j*7);}
 assert(((ext4_extent_header_t *)inode.i_block)->eh_depth==2);
 assert(stage_calls>unique&&unique>4&&!home_writes&&!flushes);
 for(unsigned b=1;b<next_block;b++)for(unsigned j=0;j<BS;j++)assert(!disk[b][j]);
 for(unsigned b=1;b<next_block;b++)if(staged[b]){memcpy(disk[b],staged[b],BS);free(staged[b]);staged[b]=NULL;}
 for(unsigned i=0;i<1600;i++)assert(ext4_get_block_num(&m,&inode,i*3)==100000+i*7);
 /* Torrent pieces arrive out of order. Verify mappings across random leaf
    and index splits, including insertions before existing index minima. */
 ext2_inode_t random_inode={0};ext4_extent_init_inode(&random_inode);
 for(unsigned i=0;i<6000;i++){
   unsigned key=(i*997+4500)%6000;
   assert(!ext4_extent_insert(&m,&random_inode,2,key*3,200000+key*7,1));
   for(unsigned j=0;j<=i;j++){
     unsigned previous=(j*997+4500)%6000;
     assert(ext4_get_block_num(&m,&random_inode,previous*3)==200000+previous*7);
   }
 }
 for(unsigned b=1;b<next_block;b++)if(staged[b]){memcpy(disk[b],staged[b],BS);free(staged[b]);staged[b]=NULL;}
 for(unsigned key=0;key<6000;key++)assert(ext4_get_block_num(&m,&random_inode,key*3)==200000+key*7);
 fail_stage=true;unsigned char node[BS]={0};assert(ext4_extent_write_node(&m,1,node)==-5);assert(!home_writes&&!flushes);
 fail_stage=false;m.journal.active=false;assert(!ext4_extent_write_node(&m,2,node));assert(home_writes==1&&flushes==1);
 printf("PASS: actual extent tree depth 2, 1600 sequential and 6000 out-of-order fragmented mappings, staged re-reads, %u updates/%u unique nodes, commit readback, error propagation and unjournaled fallback\n",stage_calls,unique);
}
