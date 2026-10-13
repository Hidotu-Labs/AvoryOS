#!/usr/bin/env python3
"""Actual cross-directory rename and replacement helper with fault injection."""
from pathlib import Path
import os
import subprocess
import tempfile

def extract(path, signature):
    s = Path(path).read_text(); a = s.index(signature); end = s.index('{', a) + 1; depth = 1
    while depth:
        depth += (s[end] == '{') - (s[end] == '}'); end += 1
    return s[a:end] + '\n'

code = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#define FS_TYPE_MASK 15
#define FS_FILE 1
#define FS_DIRECTORY 2
#define FS_SYMLINK 3
#define EXT2_FT_REG_FILE 1
#define EXT2_FT_SYMLINK 7
#define EXT4_EXTENTS_FL 0x80000
typedef struct {unsigned block_size;} ext2_mount_t;
typedef struct {unsigned i_size,i_flags;uint16_t i_links_count;uint32_t i_dtime;} ext2_inode_t;
typedef struct {void *device;unsigned inode,flags;} vfs_node_t;
typedef struct {uint32_t inode;uint16_t rec_len;uint8_t name_len,file_type;char name[];} ext2_dirent_t;
static ext2_mount_t mount={.block_size=1024};
static ext2_inode_t images[16];
static unsigned char disk[2][1024];
static unsigned depth,source_type,fail_add,fail_remove,fail_journal,fail_start,fail_stop,retired_links;
static void *kmalloc(size_t n){return malloc(n);}
static void kfree(void *p){free(p);}
static uint32_t ext2_current_time(void){return 42;}
static int ext3_journal_start(ext2_mount_t *m){assert(m==&mount);if(fail_start)return -1;assert(!depth);depth++;return 0;}
static int ext3_journal_stop(ext2_mount_t *m){assert(m==&mount && depth);depth--;return fail_stop?-1:0;}
static int ext2_read_inode(ext2_mount_t *m,unsigned ino,ext2_inode_t *out){assert(m==&mount && depth && ino<16);*out=images[ino];return 0;}
static int ext2_write_inode(ext2_mount_t *m,unsigned ino,const ext2_inode_t *in){assert(m==&mount && depth && ino<16);images[ino]=*in;retired_links++;return 0;}
static int ext2_get_block_num_checked(ext2_mount_t *m,ext2_inode_t *in,unsigned logical,unsigned *out){(void)m;assert(!(in->i_flags & EXT4_EXTENTS_FL));assert(!logical);*out=in->i_dtime+1;return 0;}
static unsigned ext4_get_block_num(ext2_mount_t *m,ext2_inode_t *in,unsigned logical){(void)m;assert(in->i_flags & EXT4_EXTENTS_FL);assert(!logical);return in->i_dtime+1;}
static int ext2_read_block(ext2_mount_t *m,unsigned block,void *out){(void)m;assert(block>=1 && block<=2);memcpy(out,disk[block-1],1024);return 0;}
static int ext3_journal_block(ext2_mount_t *m,unsigned block,const void *in){(void)m;assert(depth && block>=1 && block<=2);if(fail_journal)return -1;memcpy(disk[block-1],in,1024);return 0;}
static vfs_node_t *ext2_finddir_impl(vfs_node_t *dir,char *name){
 assert(depth);ext2_dirent_t *entry=(void *)disk[dir->inode-1];
 if(!entry->inode || strlen(name)!=entry->name_len || memcmp(entry->name,name,entry->name_len))return NULL;
 vfs_node_t *node=malloc(sizeof(*node));*node=(vfs_node_t){.device=&mount,.inode=entry->inode,.flags=entry->inode==10?source_type:FS_FILE};return node;
}
static int ext2_add_dir_entry(ext2_mount_t *m,unsigned dir,unsigned ino,const char *name,uint8_t type){
 assert(m==&mount && depth);if(fail_add)return -1;ext2_dirent_t *entry=(void *)disk[dir-1];assert(!entry->inode);
 entry->inode=ino;entry->rec_len=1024;entry->name_len=strlen(name);entry->file_type=type;memcpy(entry->name,name,entry->name_len);return 0;
}
static int ext2_remove_dir_entry(ext2_mount_t *m,unsigned dir,const char *name){
 assert(m==&mount && depth);(void)name;if(fail_remove && dir==1)return -1;ext2_dirent_t *entry=(void *)disk[dir-1];entry->inode=0;return 0;
}
static int ext2_rename_impl(vfs_node_t *dir,char *a,char *b){(void)dir;(void)a;(void)b;return 123;}
'''
code += extract('kernel/src/fs/ext2/ext2_dir.c', 'int ext2_replace_dir_entry(')
code += extract('kernel/src/fs/ext2/ext2_file.c', 'int ext2_rename_cross(')
code += r'''
static void reset(bool dest){memset(images,0,sizeof(images));memset(disk,0,sizeof(disk));depth=fail_add=fail_remove=fail_journal=fail_start=fail_stop=retired_links=0;source_type=FS_FILE;
 images[1].i_size=images[2].i_size=1024;images[2].i_dtime=1;images[10].i_links_count=images[11].i_links_count=1;
 ext2_dirent_t *e=(void *)disk[0];e->inode=10;e->rec_len=1024;e->name_len=3;e->file_type=1;memcpy(e->name,"old",3);
 e=(void *)disk[1];e->inode=dest?11:0;e->rec_len=1024;e->name_len=3;e->file_type=1;memcpy(e->name,"new",3);
}
static unsigned ino(unsigned dir){return ((ext2_dirent_t *)(void *)disk[dir-1])->inode;}
int main(void){
 vfs_node_t a={.device=&mount,.inode=1,.flags=FS_DIRECTORY},b={.device=&mount,.inode=2,.flags=FS_DIRECTORY};
 reset(false);assert(!ext2_rename_cross(&a,"old",&b,"new") && !ino(1) && ino(2)==10 && images[10].i_links_count==1 && !depth);
 reset(true);assert(!ext2_rename_cross(&a,"old",&b,"new") && !ino(1) && ino(2)==10 && !images[11].i_links_count && images[11].i_dtime==42 && !depth);
 reset(true);images[2].i_flags=EXT4_EXTENTS_FL;assert(!ext2_rename_cross(&a,"old",&b,"new") && !ino(1) && ino(2)==10 && !depth);
 reset(true);((ext2_dirent_t *)(void *)disk[1])->inode=10;assert(!ext2_rename_cross(&a,"old",&b,"new") && ino(1)==10 && ino(2)==10 && !retired_links && !depth);
 reset(false);source_type=FS_SYMLINK;assert(!ext2_rename_cross(&a,"old",&b,"new") && ((ext2_dirent_t *)(void *)disk[1])->file_type==7 && !depth);
 puts("PASS: same-mount file/symlink moves, destination replacement and same-inode no-op");
 reset(false);source_type=FS_DIRECTORY;assert(ext2_rename_cross(&a,"old",&b,"new")==-95 && ino(1)==10 && !depth);
 ext2_mount_t other;vfs_node_t c=b;c.device=&other;assert(ext2_rename_cross(&a,"old",&c,"new")==-18);
 for(int replacement=0;replacement<=1;replacement++){
  reset(replacement);fail_remove=1;assert(ext2_rename_cross(&a,"old",&b,"new")==-5 && ino(1)==10 && ino(2)==(replacement?11:0) && !retired_links && !depth);
 }
 reset(false);fail_add=1;assert(ext2_rename_cross(&a,"old",&b,"new")==-5 && ino(1)==10 && !ino(2) && !depth);
 reset(true);fail_journal=1;assert(ext2_rename_cross(&a,"old",&b,"new")==-5 && ino(1)==10 && ino(2)==11 && !depth);
 reset(false);fail_start=1;assert(ext2_rename_cross(&a,"old",&b,"new")==-5 && ino(1)==10 && !depth);
 reset(false);fail_stop=1;assert(ext2_rename_cross(&a,"old",&b,"new")==-5 && !depth);
 puts("PASS: mount/type guards, rollback on removal failure, destination I/O and journal failures");
 reset(true);depth=1;((ext2_dirent_t *)(void *)disk[1])->rec_len=4;
 assert(ext2_replace_dir_entry(&mount,2,"new",10,1)==-5 && ino(2)==11);
 depth=0;puts("PASS: malformed destination directory entries rejected before mutation");
}
'''
with tempfile.TemporaryDirectory() as tmp:
    p = Path(tmp); (p / 'test.c').write_text(code)
    subprocess.run(['cc', '-O1', '-g', '-fsanitize=address,undefined', str(p / 'test.c'), '-o', str(p / 'test')], check=True)
    subprocess.run([str(p / 'test')], check=True, env={**os.environ, 'ASAN_OPTIONS': 'detect_leaks=0'})
