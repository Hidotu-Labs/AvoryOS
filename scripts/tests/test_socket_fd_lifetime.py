#!/usr/bin/env python3
"""Exercise native socket lookup with reservation and close interleavings."""
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
#include <stdint.h>
#include <stdio.h>
#include <sys/types.h>
#define MAX_FDS 4
#define FS_TYPE_MASK 15
#define FS_SOCKET 7
#define FD_RESERVED ((vfs_node_t *)-1)
struct iovec {void *iov_base;size_t iov_len;};
struct msghdr {struct iovec *msg_iov;size_t msg_iovlen;};
struct sock_ops;
typedef struct socket {int refs;bool closing;struct sock_ops *ops;} socket_t;
struct sock_ops {ssize_t (*sendmsg)(socket_t*,struct msghdr*,int);};
typedef struct {unsigned flags; void *device;} vfs_node_t;
typedef struct {bool held;} spinlock_t;
struct fd_table {spinlock_t lock;};
struct thread {struct fd_table *files; vfs_node_t *fds[MAX_FDS];};
static struct fd_table files;
static struct thread self={.files=&files};
static socket_t socket;
static vfs_node_t node={.flags=FS_SOCKET,.device=&socket};
static void socket_put(socket_t *s);
static unsigned node_refs;
static void vfs_node_ref(vfs_node_t *n){assert(n==&node && files.lock.held);node_refs++;}
static void vfs_close(vfs_node_t *n){assert(n==&node && !files.lock.held && node_refs);node_refs--;}
static void socket_fd_put(socket_t *s){assert(node_refs);node_refs--;socket_put(s);}
static bool close_on_unlock;
static struct thread *sched_get_current(void){return &self;}
static bool fd_is_valid_node(vfs_node_t *n){return n && n!=FD_RESERVED;}
static void spinlock_acquire(spinlock_t *l){assert(!l->held);l->held=true;}
static void socket_put(socket_t *s){assert(s->refs>0);s->refs--;}
static void spinlock_release(spinlock_t *l){
 assert(l->held);l->held=false;
 if(close_on_unlock){close_on_unlock=false;self.fds[1]=NULL;socket_put(&socket);}
}
static bool socket_try_get(socket_t *s){
 assert(files.lock.held);if(!s || !s->refs)return false;s->refs++;return true;
}
'''
code += extract('kernel/src/syscalls/sys_io_shared.h', 'static inline vfs_node_t *fd_node_get(')
code += extract('kernel/src/syscalls/sys_io_shared.h', 'static inline void fd_node_put(')
code += extract('kernel/src/socket/socket.c', 'socket_t *socket_from_fd(')
code += extract('kernel/src/syscalls/sys_socket.c', 'static void socket_release_fd(')
code += extract('kernel/src/syscalls/sys_socket.c', 'static uint64_t socket_eno_notsock(')
vector_source = (ROOT / 'kernel/src/syscalls/sys_fd.c').read_text()
a = vector_source.index('  socket_t *sock = socket_from_fd((int)fd);')
b = vector_source.index('\n  size_t total = 0;', a)
code += r'''
#define kfree(p) ((void)(p))
#define fd_release_kvec(a,b) ((void)(a),(void)(b))
#define fd_release_iov(a,b) ((void)(a),(void)(b))
static ssize_t send_result;
static ssize_t send_mock(socket_t *s,struct msghdr *m,int flags){
 assert(s->refs==2 && node_refs==1 && !flags && m->msg_iovlen==1);return send_result;
}
static uint64_t vector_write(int fd){
 void *payload=NULL,*iov=NULL,*stack_iov=NULL;
 struct iovec stack_kvec[1]={{0}},*kernel_iov=stack_kvec;
 size_t iovcnt=1;
''' + vector_source[a:b] + r'''
 return (uint64_t)-88;
}
'''
code += r'''
static void syscall_return(bool early){
 socket_t *s __attribute__((cleanup(socket_release_fd))) = socket_from_fd(1);
 assert(s && s->refs==2);if(early)return;
}
int main(void){
 assert(!socket_from_fd(-1) && !socket_from_fd(MAX_FDS));
 self.fds[1]=FD_RESERVED;
 assert(!socket_from_fd(1));assert((int64_t)socket_eno_notsock(1)==-9);
 self.fds[1]=&node;node.flags=1;
 assert(!socket_from_fd(1));assert((int64_t)socket_eno_notsock(1)==-88);
 node.flags=FS_SOCKET;socket.refs=1;
 close_on_unlock=true;
 socket_t *s=socket_from_fd(1);
 assert(s==&socket && socket.refs==1 && !self.fds[1]);
 socket_fd_put(s);assert(!socket.refs && !node_refs);
 self.fds[1]=&node;socket.refs=1;
 syscall_return(false);assert(socket.refs==1);
 syscall_return(true);assert(socket.refs==1 && !node_refs);
 struct sock_ops ops={.sendmsg=send_mock};socket.ops=&ops;
 send_result=32;assert(vector_write(1)==32 && socket.refs==1 && !node_refs);
 send_result=-11;assert((int64_t)vector_write(1)==-11 && socket.refs==1 && !node_refs);
 socket.closing=true;assert((int64_t)vector_write(1)==-32 && socket.refs==1 && !node_refs);
 socket.closing=false;
 socket.refs=0;assert(!socket_from_fd(1));
 {
  vfs_node_t *p __attribute__((cleanup(fd_node_put))) = fd_node_get(&self,1);
  assert(p==&node && node_refs==1);
 }
 assert(!node_refs);self.fds[1]=FD_RESERVED;
 assert(!fd_node_get(&self,1) && !fd_node_get(&self,MAX_FDS));
 puts("PASS: reserved/non-socket descriptors, close during lookup, owned socket lifetime and all-return cleanup");
}
'''
with tempfile.TemporaryDirectory() as tmp:
    tmp = Path(tmp)
    (tmp / 'test.c').write_text(code)
    subprocess.run(['cc', '-std=gnu11', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=undefined', str(tmp / 'test.c'),
                    '-o', str(tmp / 'test')], check=True)
    subprocess.run([str(tmp / 'test')], check=True)
