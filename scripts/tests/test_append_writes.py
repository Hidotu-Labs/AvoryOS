#!/usr/bin/env python3
"""Real append syscall/VFS/transaction functions, including Firefox continuation."""
from pathlib import Path
import os
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]

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
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#define FS_FILE 1
#define FS_PIPE 3
#define FS_TYPE_MASK 0xff
#define O_APPEND 0x400
#define O_NONBLOCK 0x800
#define MAX_FDS 8
#define EXT2_WRITE_BATCH_BLOCKS 32
#define FD_IOV_STACK_MAX 16u
struct user_iovec {uint64_t iov_base,iov_len;};
#define FD_RESERVED ((vfs_node_t *)-1)
#define klogf(...) ((void)0)
typedef pthread_mutex_t spinlock_t;
static void spinlock_acquire(spinlock_t *l){pthread_mutex_lock(l);}
static void spinlock_release(spinlock_t *l){pthread_mutex_unlock(l);}
typedef struct node {
 void *device;struct node *view_backing;uint32_t flags,length;
 uint32_t (*append)(struct node *,uint32_t,uint8_t *,uint32_t *);
} vfs_node_t;
struct files {pthread_mutex_t lock;unsigned capacity;vfs_node_t *fds[MAX_FDS];uint64_t fd_offsets[MAX_FDS],fd_flags[MAX_FDS];};
struct thread {struct files *files;};
static _Thread_local struct files table={.lock=PTHREAD_MUTEX_INITIALIZER,.capacity=MAX_FDS};
static _Thread_local struct thread task;
static _Thread_local bool user;
static _Thread_local unsigned depth;
static pthread_mutex_t transaction=PTHREAD_MUTEX_INITIALIZER;
static uint8_t disk[1024*1024];
static uint32_t persistent_size, copy_limit=UINT32_MAX, write_limit=UINT32_MAX;
static bool readonly,fail_alloc,fail_start,fail_inode,fail_commit,fail_write;
static unsigned clears;
static struct thread *sched_get_current(void){task.files=&table;return &task;}
static void vfs_node_ref(vfs_node_t *n){(void)n;}
static void vfs_close(vfs_node_t *n){(void)n;}
static void trace_userspace_debug_write(struct thread *t,int fd,const void *b,size_t n){(void)t;(void)fd;(void)b;(void)n;}
static bool vfs_mount_readonly(vfs_node_t *n){(void)n;return readonly;}
static uint32_t vfs_write(vfs_node_t *n,uint32_t o,uint32_t s,uint8_t *b){
 if(fail_write)return (uint32_t)-5;
 if(s>write_limit)s=write_limit;
 if(s>UINT32_MAX-o)return (uint32_t)-27;
 assert(o+s<=sizeof(disk));memcpy(disk+o,b,s);
 if(o+s>persistent_size)persistent_size=o+s;n->length=persistent_size;return s;
}
static bool is_user_range(const void *p,uint64_t n){(void)n;return p!=NULL;}
static bool is_user_ptr(uint64_t p){(void)p;return user;}
static unsigned long copy_from_user(void *d,const void *s,unsigned long n){
 assert(!depth);unsigned long c=n<copy_limit?n:copy_limit;memcpy(d,s,c);return n-c;
}
static void *kmalloc(size_t n){assert(!depth);return fail_alloc?NULL:malloc(n);}
static void kfree(void *p){free(p);}
typedef struct {uint32_t block_size;bool read_only;struct {const char *phase;} journal;} ext2_mount_t;
typedef struct {uint32_t i_size;} ext2_inode_t;
static int ext2_read_inode(ext2_mount_t *m,unsigned ino,ext2_inode_t *i){(void)m;(void)ino;assert(depth);if(fail_inode)return -1;i->i_size=persistent_size;return 0;}
static int ext3_journal_start(ext2_mount_t *m){(void)m;if(fail_start)return -1;assert(!depth);pthread_mutex_lock(&transaction);depth=1;return 0;}
static int ext3_journal_stop(ext2_mount_t *m){(void)m;assert(depth==1);depth=0;pthread_mutex_unlock(&transaction);return fail_commit?-1:0;}
static void vfs_cache_clear(vfs_node_t *n){(void)n;clears++;}
'''.replace('uint32_t flags,length;', 'uint32_t flags,length,inode;')
fd = os.environ.get('FD_SOURCE', ROOT/'kernel/src/syscalls/sys_fd.c')
for sig in ['static vfs_node_t *fd_pin_node(', 'static void fd_advance_offset(', 'static void fd_set_offset(']:
    code += extract(ROOT/'kernel/src/syscalls/sys_fd.c', sig)
code += extract(ROOT/'kernel/src/fs/vfs.c', 'uint32_t vfs_append(')
code += extract(ROOT/'kernel/src/fs/ext2/ext2_file.c', 'uint32_t ext2_append_impl(')
code += extract(fd, 'static int64_t fd_write(')
code += extract(fd, 'static uint64_t sys_pwrite64(')
for sig in ['static int fd_snap_iov(', 'static void fd_release_iov(', 'static uint64_t sys_pwritev(']:
    code += extract(fd, sig)
code += r'''
static ext2_mount_t mount={.block_size=1024};
static void install(vfs_node_t *n,uint64_t flags,uint64_t offset){table.fds[3]=n;table.fd_flags[3]=flags;table.fd_offsets[3]=offset;}
static vfs_node_t node(void){return (vfs_node_t){.device=&mount,.flags=FS_FILE,.append=ext2_append_impl};}
static void reset(void){memset(disk,0,sizeof(disk));persistent_size=0;copy_limit=write_limit=UINT32_MAX;readonly=fail_alloc=fail_start=fail_inode=fail_commit=fail_write=false;user=false;clears=0;}
static void *writer(void *arg){
 vfs_node_t n=node();uint8_t bytes[193];memset(bytes,(int)(uintptr_t)arg,sizeof(bytes));install(&n,O_APPEND,0);user=true;
 for(unsigned i=0;i<20;i++)assert(fd_write(3,bytes,sizeof(bytes))==sizeof(bytes));return NULL;
}
int main(void){
 vfs_node_t n=node();uint8_t data[70000];for(unsigned i=0;i<sizeof(data);i++)data[i]=(unsigned char)(i*73+i/251);
 /* First chunk is already written, then Firefox renames/closes/reopens with
  * O_APPEND. A zero descriptor position must not replace that prefix. */
 reset();install(&n,0,0);assert(fd_write(3,data,21870)==21870);
 install(&n,O_APPEND,0);user=true;
 for(unsigned off=21870;off<sizeof(data);){
  int64_t w=fd_write(3,data+off,sizeof(data)-off);assert(w>0 && w<=sizeof(data)-off);off+=w;
 }
 assert(persistent_size==sizeof(data)&&!memcmp(disk,data,sizeof(data)));
 assert(table.fd_offsets[3]==sizeof(data));
 /* O_APPEND is applied on every write, including after seek and with a stale
  * alias length. F_SETFL's flag uses the same descriptor path. */
 n.length=0;table.fd_offsets[3]=0;assert(fd_write(3,data,17)==17);
 assert(table.fd_offsets[3]==sizeof(data)+17 && !memcmp(disk+sizeof(data),data,17));
 uint64_t pos=table.fd_offsets[3];assert((int64_t)sys_pwrite64(3,(uintptr_t)data,13,0,0,0)==13);
 assert(table.fd_offsets[3]==pos && persistent_size==pos+13);
 struct user_iovec vec[2]={{(uintptr_t)data,7},{(uintptr_t)(data+7),11}};
 pos=table.fd_offsets[3];uint32_t size_before=persistent_size;
 assert((int64_t)sys_pwritev(3,(uintptr_t)vec,2,0,0,0)==18);
 assert(table.fd_offsets[3]==pos && persistent_size==size_before+18);
 assert(!memcmp(disk+size_before,data,18));
 /* Non-append writes keep their requested position; non-files ignore append. */
 install(&n,0,7);assert(fd_write(3,data,9)==9&&table.fd_offsets[3]==16);
 n.flags=FS_PIPE;install(&n,O_APPEND,9);assert(fd_write(3,data,5)==5&&table.fd_offsets[3]==14);n.flags=FS_FILE;
 reset();install(&n,O_APPEND,0);user=true;copy_limit=7;
 assert(fd_write(3,data,100)==7 && table.fd_offsets[3]==7);
 copy_limit=0;assert(fd_write(3,data,100)==-14 && table.fd_offsets[3]==7);
 copy_limit=UINT32_MAX;write_limit=3;assert(fd_write(3,data,100)==3&&table.fd_offsets[3]==10);
 write_limit=UINT32_MAX;fail_alloc=true;assert(fd_write(3,data,1)==-12);fail_alloc=false;
 fail_start=true;assert(fd_write(3,data,1)==-5);fail_start=false;
 fail_inode=true;assert(fd_write(3,data,1)==-5);fail_inode=false;
 fail_write=true;assert(fd_write(3,data,1)==-5);fail_write=false;
 uint64_t before=table.fd_offsets[3];fail_commit=true;assert(fd_write(3,data,1)==-5&&clears==1&&table.fd_offsets[3]==before);fail_commit=false;
 readonly=true;assert(fd_write(3,data,1)==-30);readonly=false;
 persistent_size=UINT32_MAX;assert(fd_write(3,data,1)==-27);
 /* Independent aliases share inode EOF via journal ownership. Each write
  * must be one intact record with no overwritten/missing competing writes. */
 reset();pthread_t threads[4];for(uintptr_t i=0;i<4;i++)assert(!pthread_create(&threads[i],NULL,writer,(void *)(i+1)));
 for(unsigned i=0;i<4;i++)assert(!pthread_join(threads[i],NULL));
 assert(persistent_size==4*20*193);unsigned seen[5]={0};
 for(unsigned i=0;i<persistent_size;i+=193){unsigned id=disk[i];assert(id>=1&&id<=4);seen[id]++;for(unsigned j=1;j<193;j++)assert(disk[i+j]==id);}
 for(unsigned i=1;i<=4;i++)assert(seen[i]==20);
 puts("PASS: Firefox append continuation preserves 21870-byte prefix; EOF positions, pwrite, faults, bounds, commit errors and concurrent aliases");
}
'''
with tempfile.TemporaryDirectory(prefix='avory-append-') as tmp:
    src=Path(tmp)/'test.c';src.write_text(code)
    exe=Path(tmp)/'test'
    subprocess.run(['cc','-std=gnu11','-O1','-g','-pthread','-fsanitize=address,undefined',str(src),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
