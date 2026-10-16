#!/usr/bin/env python3
"""Run actual fd syscalls with deterministic close/dup interleavings."""
from pathlib import Path
import os
import subprocess
import tempfile

source = Path(os.environ.get('FD_SOURCE', 'kernel/src/syscalls/sys_fd.c')).read_text()

def function(signature, source=source):
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
#include <string.h>
#define MAX_FDS 16
#define FD_RESERVED ((vfs_node_t *)-1)
#define FD_FLAGS_CLOEXEC_BIT (1u << 24)
#define O_CLOEXEC 0x80000
#define O_ACCMODE 3
#define O_APPEND 0x400
#define O_NONBLOCK 0x800
#define O_RDWR 2
#define FS_TYPE_MASK 0xff
#define FS_SOCKET 2
#define FS_FILE 1
#define FS_EPOLL 3
#define FS_NONBLOCK 0x100
#define SOCK_NONBLOCK 0x800
#define F_DUPFD 0
#define F_GETFD 1
#define F_SETFD 2
#define F_GETFL 3
#define F_SETFL 4
#define F_GETLK 5
#define F_SETLK 6
#define F_SETLKW 7
#define F_SETOWN 8
#define F_DUPFD_QUERY 1027
#define F_DUPFD_CLOEXEC 1030
#define klog_puts(...) (++diagnostics)
#define klog_uint64(...) (++diagnostics)
#define sysdiag_record_unimplemented_feature(...) (++diagnostics)
static unsigned diagnostics;
typedef struct {bool held;} spinlock_t;
typedef struct {unsigned flags;} socket_t;
typedef struct {int refs; unsigned flags,impl; void *device;} vfs_node_t;
typedef struct {int refs;} eventpoll_t;
static void epoll_put(eventpoll_t *ep){assert(ep->refs>0);ep->refs--;}
struct fd_table {uint32_t next_fd; spinlock_t lock;};
struct fd_table_storage {struct fd_table table; uint64_t description_ids[MAX_FDS];};
struct thread {struct fd_table *files; vfs_node_t *fds[MAX_FDS];
uint64_t fd_offsets[MAX_FDS],fd_flags[MAX_FDS];};
static struct fd_table_storage storage;
#define table storage.table
static struct thread self = {.files=&table};
static vfs_node_t a,b;
static bool close_on_open, close_before_lock;
static int callbacks;
static uint64_t sys_close(uint64_t,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t);
static void spinlock_acquire(spinlock_t *s) {
 if(close_before_lock) {
  close_before_lock=false;
  assert(sys_close(3,0,0,0,0,0)==0);
 }
 assert(!s->held);s->held=true;
}
static void spinlock_release(spinlock_t *s){assert(s->held);s->held=false;}
static bool fd_is_valid_node(const vfs_node_t *n){return n && n!=FD_RESERVED;}
static struct thread *sched_get_current(void){return &self;}
static void vfs_node_ref(vfs_node_t *n){assert(fd_is_valid_node(n));assert(n->refs>0);n->refs++;}
static void vfs_close(vfs_node_t *n){assert(!table.lock.held);assert(fd_is_valid_node(n));assert(n->refs>0);n->refs--;}
static void vfs_open(vfs_node_t *n){
 assert(!table.lock.held);
 if(close_on_open){close_on_open=false;assert(sys_close(3,0,0,0,0,0)==0);}
 vfs_node_ref(n);callbacks++;
}
static void fd_path_dup(struct thread *t,int dst,int src){(void)t;(void)dst;(void)src;}
static void fd_path_clear(struct thread *t,int fd){(void)t;(void)fd;}
static bool asc_vfs_node_is_linux_file(void *node){(void)node;return false;}
static bool is_user_range(const void *p,size_t n){(void)p;(void)n;return true;}
'''
files_source = Path('kernel/src/sched/sched_files.c').read_text()
code += 'static uint64_t next_description_id;\n'
for name in ('uint64_t fd_description_id_locked(', 'void fd_description_set_locked('):
    code += function(name, files_source)
for signature in ['static int fd_find_free_locked(', 'int alloc_fd(', 'int alloc_fd_from(', 'static int64_t fd_query_description(', 'static uint64_t sys_close(', 'static int64_t fd_duplicate(',
                  'static uint64_t sys_dup(', 'static uint64_t sys_dup2(',
                  'static uint64_t sys_dup3(', 'static uint64_t sys_fcntl(']:
    if signature in source:
        code += function(signature)
code += function('int epoll_close_fd(', Path('kernel/src/socket/epoll.c').read_text())
code += r'''
static void reset(void) {
 memset(&self,0,sizeof(self));memset(&storage,0,sizeof(storage));self.files=&table;
 a=(vfs_node_t){.refs=1};b=(vfs_node_t){.refs=1};self.fds[3]=&a;
 self.fd_offsets[3]=42;self.fd_flags[3]=O_RDWR|FD_FLAGS_CLOEXEC_BIT;
 close_on_open=close_before_lock=false;callbacks=0;diagnostics=0;
}
int main(void) {
 reset();close_on_open=true;
 int fd=(int)sys_fcntl(3,F_DUPFD_CLOEXEC,0,0,0,0);
 assert(fd==0 && self.fds[fd]==&a && !self.fds[3] && a.refs==1);
 assert(self.fd_offsets[fd]==42 && self.fd_flags[fd]==(O_RDWR|FD_FLAGS_CLOEXEC_BIT));
 int next=(int)sys_fcntl(fd,F_DUPFD_CLOEXEC,0,0,0,0);
 assert(next==1 && self.fds[next]==&a && a.refs==2);
 assert(sys_close(fd,0,0,0,0,0)==0);assert(sys_close(next,0,0,0,0,0)==0);assert(a.refs==0);
 reset();close_before_lock=true;
 assert((int64_t)sys_dup(3,0,0,0,0,0)==-9);assert(!self.fds[0] && !callbacks);
 reset();self.fds[0]=FD_RESERVED;
 assert((int64_t)sys_close(0,0,0,0,0,0)==-9 && self.fds[0]==FD_RESERVED);
 assert((int64_t)sys_dup2(3,0,0,0,0,0)==-16 && self.fds[0]==FD_RESERVED);
 reset();self.fds[7]=&b;
 assert(sys_dup2(3,7,0,0,0,0)==7 && self.fds[7]==&a && b.refs==0 && a.refs==2);
 assert(self.fd_flags[7]==O_RDWR && self.fd_offsets[7]==42);
 assert(sys_dup2(3,3,0,0,0,0)==3 && a.refs==2);
 assert((int64_t)sys_dup3(3,3,0,0,0,0)==-22);
 assert(sys_dup3(3,8,O_CLOEXEC,0,0,0)==8 && self.fd_flags[8]==(O_RDWR|FD_FLAGS_CLOEXEC_BIT));
 reset();assert((int64_t)sys_fcntl(3,F_DUPFD,UINT32_MAX,0,0,0)==-22);
 assert((int64_t)sys_fcntl(3,F_DUPFD,MAX_FDS,0,0,0)==-22);
 for(int i=0;i<MAX_FDS;i++)self.fds[i]=FD_RESERVED;
 self.fds[3]=&a;
 assert((int64_t)sys_fcntl(3,F_DUPFD,0,0,0,0)==-24);
 reset();table.next_fd=MAX_FDS;
 assert(sys_fcntl(3,F_DUPFD_CLOEXEC,7,0,0,0)==7 && table.next_fd==0);
 assert(sys_dup(3,0,0,0,0,0)==0);
 reset();self.fds[3]=FD_RESERVED;
 assert((int64_t)sys_fcntl(3,F_DUPFD_CLOEXEC,0,0,0,0)==-9);
 reset();eventpoll_t ep={.refs=1};b.flags=FS_EPOLL;b.device=&ep;
 self.fds[7]=&b;self.fd_offsets[7]=11;self.fd_flags[7]=FD_FLAGS_CLOEXEC_BIT;
 table.next_fd=MAX_FDS;
 assert(epoll_close_fd(7)==0 && !self.fds[7] && !self.fd_offsets[7]);
 assert(!self.fd_flags[7] && table.next_fd==7 && !b.refs && !ep.refs);
 self.fds[7]=FD_RESERVED;
 assert(epoll_close_fd(7)==-9 && self.fds[7]==FD_RESERVED);
 /* Same inode, flags and offsets still mean distinct independent opens. */
 reset();self.fds[4]=&a;a.refs++;
 self.fd_offsets[4]=self.fd_offsets[3];self.fd_flags[4]=self.fd_flags[3];
 assert(sys_fcntl(3,F_DUPFD_QUERY,3,0,0,0)==1);
 assert(sys_fcntl(3,F_DUPFD_QUERY,4,0,0,0)==0);
 assert(sys_dup3(3,7,O_CLOEXEC,0,0,0)==7);
 assert(sys_fcntl(3,F_DUPFD_QUERY,7,0,0,0)==1);
 assert(sys_fcntl(7,F_DUPFD_QUERY,3,0,0,0)==1);
 assert(sys_fcntl(4,F_DUPFD_QUERY,7,0,0,0)==0);
 assert(sys_fcntl(3,F_DUPFD_QUERY,(1ULL<<32)|7,0,0,0)==1);
 assert(sys_fcntl((1ULL<<32)|3,(1ULL<<32)|F_DUPFD_QUERY,7,0,0,0)==1);
 for(int i=0;i<10000;i++) assert(sys_fcntl(3,F_DUPFD_QUERY,7,0,0,0)==1);
 assert(!diagnostics);
 assert((int64_t)sys_fcntl(3,F_DUPFD_QUERY,UINT32_MAX,0,0,0)==-9);
 assert((int64_t)sys_fcntl(3,F_DUPFD_QUERY,MAX_FDS,0,0,0)==-9);
 assert((int64_t)sys_fcntl(MAX_FDS,F_DUPFD_QUERY,3,0,0,0)==-9);
 assert((int64_t)sys_fcntl(3,F_DUPFD_QUERY,0,0,0,0)==-9);
 self.fds[0]=FD_RESERVED;
 assert((int64_t)sys_fcntl(0,F_DUPFD_QUERY,3,0,0,0)==-9);
 assert((int64_t)sys_fcntl(3,F_DUPFD_QUERY,0,0,0,0)==-9);
 self.fds[0]=NULL;
 /* Reopening the same node in a recycled slot is a new description. */
 uint64_t old_id=fd_description_id_locked(&self,3);
 assert(sys_close(3,0,0,0,0,0)==0);
 assert(alloc_fd_from(&self,3)==3);self.fds[3]=&a;a.refs++;
 assert(sys_fcntl(3,F_DUPFD_QUERY,7,0,0,0)==0);
 assert(fd_description_id_locked(&self,3)!=old_id);
 assert(sys_dup2(7,3,0,0,0,0)==3);
 assert(sys_fcntl(3,F_DUPFD_QUERY,7,0,0,0)==1);
 /* Validation and comparison see one locked snapshot. */
 reset();close_before_lock=true;
 assert((int64_t)sys_fcntl(3,F_DUPFD_QUERY,3,0,0,0)==-9);
 assert(!diagnostics);
 reset();assert((int64_t)sys_fcntl(3,0xffff,0,0,0,0)==-22);
 assert(diagnostics);
 puts("fd duplication/close/F_DUPFD_QUERY regression: PASS");
}
'''
with tempfile.TemporaryDirectory(prefix='avory-fd-test-') as tmp:
    src = Path(tmp) / 'test.c'
    exe = Path(tmp) / 'test'
    src.write_text(code)
    subprocess.run(['cc', '-std=gnu11', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=undefined', str(src), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
