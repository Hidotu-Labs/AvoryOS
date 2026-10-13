#!/usr/bin/env python3
"""Exercise real ext4 readdir result lifetime and unlink descriptor semantics."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]

def function(file, signature):
    text = (root / file).read_text()
    start = text.index(signature)
    end = text.index('{', start) + 1
    depth = 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[start:end] + '\n'

code = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define MAX_FDS 16
#define FS_TYPE_MASK 0xff
#define FS_DIRECTORY 2
#define FS_FILE 1
#define DT_UNKNOWN 0
#define DT_REG 8
#define DT_DIR 4
#define DT_CHR 2
#define DT_BLK 6
#define DT_FIFO 1
#define DT_SOCK 12
#define DT_LNK 10
struct dirent {char name[128]; uint32_t ino; uint8_t d_type;};
typedef struct {bool held;} spinlock_t;
typedef struct vfs_node {
 unsigned flags,inode; void *device; int refs;
 spinlock_t readdir_cursor_lock;
 uint32_t readdir_cursor_index,readdir_cursor_offset;
} vfs_node_t;
struct fd_table {unsigned next_fd;};
struct thread {
 struct dirent readdir_result;
 struct fd_table *files;
 vfs_node_t *fds[MAX_FDS],*cwd_node;
 uint64_t fd_offsets[MAX_FDS],fd_flags[MAX_FDS];
};
static struct thread a,b,*current;
static struct fd_table table;
static vfs_node_t parent,target;
static vfs_node_t *fs_root=&parent;
static struct thread *sched_get_current(void){return current;}
static void spinlock_acquire(spinlock_t *s){assert(!s->held);s->held=true;}
static void spinlock_release(spinlock_t *s){assert(s->held);s->held=false;}
static void *kmalloc(size_t n){return calloc(1,n);}
static void kfree(void *p){free(p);}
typedef struct {uint32_t block_size; const char *name; uint32_t ino;} ext2_mount_t;
typedef struct {uint32_t i_size;} ext2_inode_t;
typedef struct __attribute__((packed)) {
 uint32_t inode; uint16_t rec_len; uint8_t name_len,file_type; char name[];
} ext2_dirent_t;
static int ext2_read_inode(ext2_mount_t *m,unsigned ino,ext2_inode_t *out){
 (void)ino;out->i_size=m->block_size;return 0;
}
static uint32_t ext2_get_block_num(ext2_mount_t *m,ext2_inode_t *ino,unsigned i){
 (void)m;(void)ino;return i==0 ? 1 : 0;
}
static void ext2_read_block(ext2_mount_t *m,unsigned block,void *buf){
 assert(block==1);ext2_dirent_t *d=buf;d->inode=m->ino;d->rec_len=m->block_size;
 d->name_len=strlen(m->name);d->file_type=1;memcpy(d->name,m->name,d->name_len);
}
static vfs_node_t *resolve_parent_and_name(const char *path,char *name,size_t size){
 assert(!strcmp(path,"/tmp/wine-file"));assert(size>9);strcpy(name,"wine-file");
 parent.refs++;return &parent;
}
static vfs_node_t *vfs_finddir(vfs_node_t *p,char *name){
 assert(p==&parent && !strcmp(name,"wine-file"));target.refs++;return &target;
}
static bool vfs_may_remove(vfs_node_t *p,vfs_node_t *n){
 assert(p==&parent && n==&target);return true;
}
static void unix_unbind_by_path(const char *path){(void)path;}
static void vfs_close(vfs_node_t *n){assert(n->refs>0);n->refs--;}
static int vfs_unlink(vfs_node_t *p,char *name){
 assert(p==&parent && !strcmp(name,"wine-file"));
 vfs_close(&target);return 0; /* drop only directory ownership */
}
static void vfs_path_cache_drop(vfs_node_t *p,const char *path){(void)p;(void)path;}
'''
code += function('kernel/src/fs/vfs.c', 'struct dirent *vfs_readdir_buffer(')
code += function('kernel/src/fs/ext2/ext2_dir.c', 'static uint8_t ext2_dirent_dtype(')
code += function('kernel/src/fs/ext2/ext2_dir.c', 'struct dirent *ext2_readdir_impl(')
code += function('kernel/src/syscalls/sys_fs.c', 'static uint64_t sys_unlink(')
code += r'''
int main(void){
 ext2_mount_t ma={.block_size=1024,.name="ole32.dll",.ino=123};
 ext2_mount_t mb={.block_size=1024,.name="font.ttf",.ino=456};
 vfs_node_t da={.device=&ma},db={.device=&mb};
 current=&a;struct dirent *first=ext2_readdir_impl(&da,0);
 assert(first && !strcmp(first->name,"ole32.dll") && first->ino==123);
 /* Pause thread A before getdents copies its result. Thread B lists a
    different directory, so even a per-node cursor lock cannot protect A. */
 current=&b;struct dirent *second=ext2_readdir_impl(&db,0);
 assert(second && !strcmp(second->name,"font.ttf") && second->ino==456);
 assert(first!=second && !strcmp(first->name,"ole32.dll") && first->ino==123);
 current=&a;assert(vfs_readdir_buffer()==first); /* CPU migration irrelevant */
 assert(ext2_readdir_impl(&da,1)==NULL);
 parent=(vfs_node_t){.flags=FS_DIRECTORY,.refs=1};
 target=(vfs_node_t){.flags=FS_FILE,.refs=3}; /* directory plus two fds */
 a.files=&table;table.next_fd=MAX_FDS;a.fds[3]=a.fds[5]=&target;
 a.fd_offsets[3]=42;a.fd_flags[3]=0x800;
 assert(sys_unlink((uintptr_t)"/tmp/wine-file",0,0,0,0,0)==0);
 assert(a.fds[3]==&target && a.fds[5]==&target && target.refs==2);
 assert(a.fd_offsets[3]==42 && a.fd_flags[3]==0x800 && table.next_fd==MAX_FDS);
 assert(parent.refs==1);
 vfs_close(a.fds[3]);vfs_close(a.fds[5]);assert(target.refs==0);
 puts("PASS: interleaved ext4 directory entries, thread-local result storage, unlink preserves open descriptors and ownership");
}
'''
with tempfile.TemporaryDirectory(prefix='avory-wine-dir-') as tmp:
    p = Path(tmp)
    (p/'test.c').write_text(code)
    subprocess.run(['cc', '-Wall', '-Wextra', '-Werror', '-fsanitize=undefined',
                    str(p/'test.c'), '-o', str(p/'test')], check=True)
    subprocess.run([str(p/'test')], check=True)
