#!/usr/bin/env python3
"""Actual VFS/syscall preallocation code: size persistence and narrowing checks."""
from pathlib import Path
import os
import subprocess
import tempfile


def extract(path, signature):
    source = Path(path).read_text()
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
#include <stddef.h>
#include <stdio.h>
#define FS_TYPE_MASK 0xff
#define FS_FILE 1
#define FS_PAGE_CACHE 0x100
#define MAX_FDS 8
#define klogf(...) fprintf(stderr, __VA_ARGS__)
typedef struct vfs_node {
 uint32_t flags, length;
 bool mmap_shared_only;
 int (*truncate)(struct vfs_node *, uint32_t);
 int (*fallocate)(struct vfs_node *, int, uint32_t, uint32_t);
} vfs_node_t;
struct fd_table {int lock;vfs_node_t *fds[MAX_FDS];};
struct thread {struct fd_table *files;char comm[16];unsigned tid;};
static struct fd_table table;
static struct thread current={.files=&table};
static vfs_node_t file;
static uint32_t persisted_size=17;
static unsigned truncates,allocates,closes,cache_clears;
static uint32_t seen_offset,seen_len;
static int seen_mode;
static struct thread *sched_get_current(void){return &current;}
static void spinlock_acquire(int *p){(void)p;}
static void spinlock_release(int *p){(void)p;}
static vfs_node_t *fd_pin_node(struct thread *t,uint64_t fd,void *a,void *b){
 (void)t;(void)a;(void)b;return fd==3?&file:NULL;
}
static void vfs_close(vfs_node_t *n){assert(n==&file);closes++;}
static void vfs_cache_clear(vfs_node_t *n){(void)n;cache_clears++;}
static int resize(vfs_node_t *n,uint32_t z){truncates++;n->length=persisted_size=z;return 0;}
static int reserve(vfs_node_t *n,int m,uint32_t o,uint32_t z){
 (void)n;allocates++;seen_mode=m;seen_offset=o;seen_len=z;return 0;
}
'''
vfs = os.environ.get('VFS_SOURCE', 'kernel/src/fs/vfs.c')
fd = os.environ.get('FD_SOURCE', 'kernel/src/syscalls/sys_fd.c')
code += extract(vfs, 'int vfs_truncate(')
code += extract(vfs, 'int vfs_fallocate(')
code += extract(fd, 'static uint64_t sys_ftruncate(')
code += extract(fd, 'static uint64_t sys_fallocate(')
code += r'''
static int64_t allocate(uint64_t mode,uint64_t offset,uint64_t len){
 return (int64_t)sys_fallocate(3,mode,offset,len,0,0);
}
int main(void){
 file=(vfs_node_t){.flags=FS_FILE|FS_PAGE_CACHE,.length=17,.truncate=resize};
 /* Simulate a partial download already containing 17 bytes. Neither request
  * may resize its inode or claim that disk blocks have been reserved. */
 int64_t result=allocate(1,0,1000000);
 assert(file.length==17 && persisted_size==17 && !truncates);
 assert(result==-95);
 assert(allocate(0,0,1000000)==-95);
 assert(file.length==17 && persisted_size==17 && !truncates);
 assert(allocate(0,0,0)==-22);
 assert(allocate(0,UINT64_MAX,1)==-22);
 assert(allocate(0,0,UINT64_MAX)==-22);
 assert(allocate(0,0,UINT64_C(1)<<32)==-27);
 assert(allocate(1,UINT32_MAX,1)==-27);
 assert(allocate(0,UINT64_C(1)<<32,1)==-27);
 assert(allocate(UINT64_C(1)<<32,0,100)==-95);
 assert(allocate(3,0,100)==-95);
 assert((int64_t)sys_ftruncate(3,(UINT64_C(1)<<32)+17,0,0,0,0)==-27);
 assert(file.length==17 && persisted_size==17 && !truncates);
 file.fallocate=reserve;
 assert(allocate(1,19,100)==0 && allocates==1);
 assert(seen_mode==1 && seen_offset==19 && seen_len==100);
 assert(allocate(0,0,UINT32_MAX)==0 && allocates==2);
 assert(allocate(0,UINT32_MAX,1)==-27 && allocates==2);
 assert(vfs_fallocate(&file,0,UINT32_MAX,1)==-27 && allocates==2);
 assert(vfs_fallocate(&file,0,0,0)==-22 && allocates==2);
 assert(!sys_ftruncate(3,123,0,0,0,0));
 assert(file.length==123 && persisted_size==123 && truncates==1 && cache_clears==1);
 /* Tmpfs private maps own cache copies even though read() bypasses cache. */
 file.flags=FS_FILE;file.mmap_shared_only=true;
 assert(!vfs_truncate(&file,99));assert(cache_clears==2);
 assert(closes==15);
 puts("PASS: unsupported reservation preserves persistent size; large ranges never wrap; native reservation and truncate still work");
}
'''
with tempfile.TemporaryDirectory(prefix='avory-download-') as tmp:
    src = Path(tmp) / 'test.c'
    exe = Path(tmp) / 'test'
    src.write_text(code)
    subprocess.run(['cc', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=undefined', str(src), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
