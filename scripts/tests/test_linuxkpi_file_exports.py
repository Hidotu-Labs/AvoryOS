#!/usr/bin/env python3
"""Run the native fd bridge against repeated and overlapping dma-buf exports."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
def extract(path, signature):
    source = (ROOT / path).read_text()
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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#define MAX_FDS 4
#define KPI_O_CLOEXEC 0x80000u
#define KPI_FD_FLAGS_CLOEXEC_BIT (1u<<24)
#define KPI_FD_RESERVED ((vfs_node_t*)-1)
#define FD_RESERVED KPI_FD_RESERVED
struct file;
typedef struct vfs_node {
 void *device,*wait_queue; unsigned flags,mask;
 uint64_t length,inode; bool mmap_shared_only,mmap_private_pmm_refs;
 char name[32]; int refs;
} vfs_node_t;
struct fd_table {int lock;unsigned next_fd;vfs_node_t *reserved_nodes[MAX_FDS];};
struct thread {struct fd_table *files;vfs_node_t *fds[MAX_FDS];
 uint64_t fd_offsets[MAX_FDS],fd_flags[MAX_FDS];};
struct inode {uint64_t i_size;};
struct file {int f_count;unsigned f_flags;void *f_asc_node;struct inode *f_inode;};
static struct fd_table files;
static struct thread self={.files=&files};
static unsigned live_nodes,releases;
static bool oom;
static struct thread *sched_get_current(void){return &self;}
static void spinlock_acquire(int *p){assert(!*p);*p=1;}
static void spinlock_release(int *p){assert(*p);*p=0;}
static void atomic_inc(int *p){assert(*p>0);++*p;}
static bool atomic_dec_and_test(int *p){assert(*p>0);return --*p==0;}
static void fput(struct file *);
static void linuxkpi_file_close(void *);
static void *asc_vfs_node_device(void *n){return ((vfs_node_t*)n)->device;}
static void vfs_close(vfs_node_t *n){
 assert(!files.lock && n && n!=FD_RESERVED && n->refs>0);
 if(--n->refs)return;
 linuxkpi_file_close(n);live_nodes--;free(n);
}
static void vfs_node_ref(vfs_node_t *n){assert(n && n!=FD_RESERVED && n->refs>0);n->refs++;}
static void asc_vfs_node_unref(void *n){vfs_close(n);}
static bool asc_vfs_node_is_linux_file(void *n){return n && ((vfs_node_t*)n)->device;}
static void *asc_vfs_anon_node(void){
 if(oom)return NULL;
 vfs_node_t *n=calloc(1,sizeof(*n));assert(n);n->refs=1;live_nodes++;return n;
}
static void kpi_file_free(struct file *f){
 /* The owner's private reference must survive all descriptor closes. */
 assert(f->f_asc_node && ((vfs_node_t*)f->f_asc_node)->device==f);
 vfs_node_t *n=f->f_asc_node;n->device=NULL;vfs_close(n);f->f_asc_node=NULL;releases++;
}
static int alloc_fd(struct thread *t){
 for(int i=0;i<MAX_FDS;i++)if(!t->fds[i]){t->fds[i]=FD_RESERVED;return i;}
 return -1;
}
static void fd_path_clear(struct thread *t,int fd){(void)t;(void)fd;}
static void asc_vfs_node_set_size(void *n,uint64_t z){((vfs_node_t*)n)->length=z;}
'''
code += extract('kernel/linuxkpi/src/file.c', 'struct file *get_file(')
code += extract('kernel/linuxkpi/src/file.c', 'void fput(')
code += extract('kernel/linuxkpi/src/file.c', 'static struct file *kpi_node_file(')
code += extract('kernel/linuxkpi/src/file.c', 'void linuxkpi_file_close(')
code += extract('kernel/src/syscalls/sys_fd.c', 'void fd_release_reserved(')
for signature in ['int asc_vfs_fd_alloc(', 'void asc_vfs_fd_release_reserved(',
                  'int asc_vfs_file_fd_alloc(', 'void asc_vfs_file_fd_install(',
                  'void *asc_vfs_fd_get_node(']:
    code += extract('kernel/src/linuxkpi/native_vfs.c', signature)
for signature in ['int get_unused_fd_flags(', 'void put_unused_fd(', 'void fd_install(', 'struct file *fget(']:
    code += extract('kernel/linuxkpi/src/file.c', signature)
code += r'''
static int export(struct file *f,unsigned flags){
 int fd=get_unused_fd_flags(flags);assert(fd>=0);get_file(f);fd_install(fd,f);
 assert(self.fds[fd] && self.fds[fd]!=FD_RESERVED);
 assert(!files.reserved_nodes[fd]);return fd;
}
static void close_export(int fd){vfs_node_t *n=self.fds[fd];self.fds[fd]=NULL;vfs_close(n);}
int main(void){
 struct inode ino={.i_size=8192};struct file f={.f_count=1,.f_flags=KPI_O_CLOEXEC,.f_inode=&ino};
 f.f_asc_node=asc_vfs_anon_node();vfs_node_t *owner=f.f_asc_node;
 owner->device=&f;owner->flags=42;owner->wait_queue=&ino;strcpy(owner->name,"dmabuf");
 for(int i=0;i<100;i++){
   int a=export(&f,KPI_O_CLOEXEC);vfs_node_t *wrapper=self.fds[a];
   assert(wrapper!=owner && wrapper->length==8192 && wrapper->flags==42);
   assert(wrapper->wait_queue==&ino && strcmp(wrapper->name,"dmabuf")==0);
   assert(self.fd_flags[a]==KPI_FD_FLAGS_CLOEXEC_BIT);
   int b=export(&f,0);assert(self.fds[b]!=wrapper && f.f_count==3);
   assert(self.fd_flags[b]==0);
   assert(fget(a)==&f && f.f_count==4);fput(&f);assert(f.f_count==3);
   /* dup/SCM_RIGHTS pins the wrapper, retaining its transferred file ref. */
   wrapper->refs++;close_export(a);assert(f.f_count==3);
   close_export(b);assert(f.f_count==2 && f.f_asc_node==owner);
   vfs_close(wrapper);assert(f.f_count==1 && f.f_asc_node==owner && live_nodes==1);
 }
 unsigned before=live_nodes;oom=true;
 assert(get_unused_fd_flags(0)==-12 && live_nodes==before);oom=false;
 int reserved[MAX_FDS];
 for(int i=0;i<MAX_FDS;i++){reserved[i]=get_unused_fd_flags(0);assert(fget(reserved[i])==NULL);}
 assert(live_nodes==1+MAX_FDS && get_unused_fd_flags(0)==-24 && live_nodes==1+MAX_FDS);
 for(int i=0;i<MAX_FDS;i++)put_unused_fd(reserved[i]);
 assert(live_nodes==1 && f.f_count==1);
 fput(&f);assert(releases==1 && live_nodes==0);
 puts("PASS: repeated/overlapping DMA-buffer exports, transfer lifetime, reservation cancellation, OOM and exhaustion");
}
'''
with tempfile.TemporaryDirectory(prefix='avory-file-exports-') as tmp:
    source=Path(tmp)/'test.c'; binary=Path(tmp)/'test'
    source.write_text(code)
    subprocess.run(['cc','-std=gnu11','-Wall','-Wextra','-Werror',str(source),'-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True)
