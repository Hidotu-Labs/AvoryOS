#!/usr/bin/env python3
"""Exercise actual table allocation, inheritance, pidfd transfer and teardown."""
from pathlib import Path
import os
import subprocess
import tempfile


def block(text, signature):
    start = text.index(signature)
    end = text.index('{', start) + 1
    depth = 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[start:end]


sched = Path('kernel/src/sched/sched_files.c').read_text()
header = Path('kernel/src/sched/sched.h').read_text()
fd = Path('kernel/src/syscalls/sys_fd.c').read_text()
process = Path('kernel/src/syscalls/sys_process.c').read_text()
assert process.count('sched_copy_files(child, parent);') == 3
code = r'''
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#define MAX_FDS 16
#define FD_RESERVED ((vfs_node_t *)-1)
#define FD_FLAGS_CLOEXEC_BIT (1ULL<<24)
#define klog_debug_puts(...) ((void)0)
#define klog_debug_hex64(...) ((void)0)
#define klog_debug_uint64(...) ((void)0)
typedef struct {bool held;} spinlock_t;
typedef struct vfs_node {int refcount;char name[32];void *device;} vfs_node_t;
'''
code += block(header, 'struct fd_path {') + ';\n'
code += block(header, 'struct fd_table {') + ';\n'
code += block(sched, 'struct fd_table_storage {') + ';\n'
code += r'''
struct thread {
 struct fd_table *files;
 vfs_node_t **fds;
 uint64_t *fd_offsets,*fd_flags;
 struct fd_path **fd_paths;
 bool is_forked_child;
 uint32_t tid,tgid;
 struct thread *global_next;
};
static struct thread *current, *global_thread_list, *close_source;
static spinlock_t tid_lock;
static bool fail_alloc;
static int allocations;
static uint64_t next_description_id;
static void *kmalloc(size_t n){if(fail_alloc)return NULL;void *p=malloc(n);assert(p);allocations++;return p;}
static void kfree(void *p){if(p){allocations--;free(p);}}
static void spinlock_init(spinlock_t *s){s->held=false;}
static void spinlock_acquire(spinlock_t *s){assert(!s->held);s->held=true;}
static void spinlock_release(spinlock_t *s){assert(s->held);s->held=false;}
static bool asc_vfs_node_is_linux_file(void *node){(void)node;return false;}
static bool fd_is_valid_node(vfs_node_t *n){return n && n!=FD_RESERVED;}
static struct thread *sched_get_current(void){return current;}
static void vfs_node_ref(vfs_node_t *n){assert(fd_is_valid_node(n) && n->refcount>0);n->refcount++;}
static void vfs_close(vfs_node_t *n){assert(n->refcount>0);n->refcount--;}
static void vfs_open(vfs_node_t *n){
 assert(!tid_lock.held);
 if(close_source){
  struct thread *t=close_source;close_source=NULL;
  assert(!t->files->lock.held);
  spinlock_acquire(&t->files->lock);
  vfs_node_t *old=t->fds[3];t->fds[3]=NULL;
  spinlock_release(&t->files->lock);
  vfs_close(old); // close/reuse during filesystem callback
 }
 vfs_node_ref(n);
}
struct pidfd_ctx {uint32_t pid;};
static struct pidfd_ctx *pidfd_ctx_of(vfs_node_t *n){return fd_is_valid_node(n)?n->device:NULL;}
void sched_release_files(struct thread *t);
'''
for signature in ('uint64_t fd_description_id_locked(', 'void fd_description_set_locked(',
                  'static struct fd_table *fd_table_create(', 'static void thread_set_files(',
                  'static void fd_path_put(', 'void fd_path_clear(',
                  'bool sched_ensure_files(', 'void sched_release_files(',
                  'void sched_share_files(', 'void sched_copy_files('):
    code += block(sched, signature) + '\n'
for signature in ('static int fd_find_free_locked(', 'int alloc_fd(',
                  'static int64_t fd_query_description('):
    code += block(fd, signature) + '\n'
code += block(process, 'static uint64_t sys_pidfd_getfd(') + '\n'
code += r'''
int main(void){
 struct thread parent={.tid=11,.tgid=11},child={0},shared={0};
 fail_alloc=true;assert(!sched_ensure_files(&parent) && !parent.files);
 fail_alloc=false;assert(sched_ensure_files(&parent));
 assert(sched_ensure_files(&child) && sched_ensure_files(&shared));
 assert(allocations==3);
 vfs_node_t node={.refcount=2},reserved={.refcount=1};
 parent.fds[3]=parent.fds[4]=&node;
 parent.fd_offsets[3]=345;parent.fd_flags[3]=0x800|FD_FLAGS_CLOEXEC_BIT;
 uint64_t identity=fd_description_id_locked(&parent,3);
 fd_description_set_locked(&parent,4,identity);
 parent.fds[7]=FD_RESERVED;parent.files->reserved_nodes[7]=&reserved;
 struct fd_path *path=kmalloc(sizeof(*path));path->ref_count=2;
 parent.fd_paths[3]=parent.fd_paths[4]=path;
 sched_copy_files(&child,&parent);
 assert(node.refcount==4 && path->ref_count==4);
 assert(!child.fds[7] && !child.files->reserved_nodes[7]);
 assert(fd_query_description(&child,3,4)==1);
 assert(fd_description_id_locked(&child,3)==identity);
 assert(child.fd_offsets[3]==345 && child.fd_flags[3]==parent.fd_flags[3]);
 sched_share_files(&shared,&parent);assert(allocations==3); // two tables + path
 assert(shared.files==parent.files && parent.files->ref_count==2);
 assert(fd_query_description(&shared,3,4)==1);
 sched_release_files(&shared);assert(!shared.files && parent.files->ref_count==1);
 sched_release_files(&child);assert(node.refcount==2 && path->ref_count==2);
 assert(allocations==2);
 /* A close during a callback cannot invalidate the pinned inheritance snapshot. */
 assert(sched_ensure_files(&child));
 close_source=&parent;sched_copy_files(&child,&parent);
 assert(!parent.fds[3] && child.fds[3]==&node && node.refcount==3);
 assert(fd_query_description(&child,3,4)==1);
 // Account for the path of the simulated source close, as sys_close does.
 fd_path_clear(&parent,3);
 /* pidfd_getfd inherits description identity and CLOEXEC; independent reuse does not. */
 current=&child;global_thread_list=&parent;
 struct pidfd_ctx ctx={.pid=11};vfs_node_t pidnode={.refcount=1,.device=&ctx};
 child.fds[10]=&pidnode;
 int out=(int)sys_pidfd_getfd(10,4,0,0,0,0);
 assert(out==0 && child.fds[out]==&node && node.refcount==4);
 assert(fd_query_description(&child,out,3)==1);
 assert(child.fd_flags[out]&FD_FLAGS_CLOEXEC_BIT);
 assert((int64_t)sys_pidfd_getfd(10,7,0,0,0,0)==-9);
 assert((int64_t)sys_pidfd_getfd(10,4,1,0,0,0)==-22);
 // No free descriptor: the transfer reference must be released on failure.
 for(int i=0;i<MAX_FDS;i++)if(!child.fds[i])child.fds[i]=FD_RESERVED;
 assert((int64_t)sys_pidfd_getfd(10,4,0,0,0,0)==-24 && node.refcount==4);
 sched_release_files(&child);assert(node.refcount==1 && pidnode.refcount==0);
 sched_release_files(&parent);assert(node.refcount==0 && reserved.refcount==0);
 assert(!allocations);
 puts("PASS: real fd-table allocation/layout, fork/clone/vfork snapshot, CLONE_FILES, callback close, pidfd identity, teardown/OOM");
}
'''
with tempfile.TemporaryDirectory(prefix='avory-fd-lifetime-') as tmp:
    source = Path(tmp) / 'test.c'
    exe = Path(tmp) / 'test'
    source.write_text(code)
    subprocess.run(['cc', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
                    str(source), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True, env={**os.environ, 'ASAN_OPTIONS': 'detect_leaks=1'})
