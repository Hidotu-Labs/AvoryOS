#!/usr/bin/env python3
"""Exercise real SCM metadata, fchdir, stream-boundary and fd-allocation code."""
from pathlib import Path
import os
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
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#define MAX_FDS 16
#define FD_RESERVED ((vfs_node_t *)-1)
#define FD_FLAGS_CLOEXEC_BIT (1u << 24)
#define MSG_CMSG_CLOEXEC 0x40000000
#define FS_TYPE_MASK 0xff
#define FS_DIRECTORY 2
#define O_NONBLOCK 0x800
#define klog_puts(...) ((void)0)
#define klog_uint64(...) ((void)0)
typedef struct { bool held; } spinlock_t;
typedef struct { unsigned flags; int refs; } vfs_node_t;
struct fd_table { spinlock_t lock; uint32_t next_fd; vfs_node_t *reserved_nodes[MAX_FDS]; };
struct thread {
 struct fd_table *files;
 vfs_node_t *fds[MAX_FDS], *cwd_node;
 uint64_t fd_offsets[MAX_FDS], fd_flags[MAX_FDS];
 char paths[MAX_FDS][256], cwd_path[256];
};
typedef struct {vfs_node_t *node; uint64_t offset, flags; char path[256];} unix_scm_fd_t;
typedef struct unix_scm_msg {struct unix_scm_msg *next; size_t stream_offset; int count; unix_scm_fd_t fds[];} unix_scm_msg_t;
typedef struct {size_t bytes_read; unix_scm_msg_t *scm_queue_head, *scm_queue_tail;} unix_sock_t;
static struct thread *current;
static bool path_oom;
static void spinlock_acquire(spinlock_t *l) { assert(!l->held); l->held=true; }
static void spinlock_release(spinlock_t *l) { assert(l->held); l->held=false; }
static bool fd_is_valid_node(vfs_node_t *n) {return n && n != FD_RESERVED;}
static void vfs_node_ref(vfs_node_t *n) {assert(fd_is_valid_node(n) && n->refs > 0); n->refs++;}
static void vfs_open(vfs_node_t *n) {vfs_node_ref(n);}
static void vfs_close(vfs_node_t *n) {assert(fd_is_valid_node(n) && n->refs > 0); n->refs--;}
static const char *fd_path_value(struct thread *t,int fd) {return t->paths[fd][0] ? t->paths[fd] : NULL;}
static bool fd_path_set(struct thread *t,int fd,const char *path) {if(path_oom)return false; strcpy(t->paths[fd],path); return true;}
static void fd_path_clear(struct thread *t,int fd) {t->paths[fd][0]=0;}
static struct thread *sched_get_current(void) {return current;}
static bool vfs_access(vfs_node_t *n,int mask) {(void)n;(void)mask;return true;}
static void kfree(void *p) {free(p);}
'''
for name in ('static bool fd_make_path(', 'static int fd_find_free_locked(', 'int alloc_fd(', 'int alloc_fd_from(', 'void fd_release_reserved('):
    code += function('kernel/src/syscalls/sys_fd.c', name)
for name in ('static bool unix_scm_capture_fd(', 'static int unix_scm_install_fd(', 'static void unix_discard_consumed_scm('):
    code += function('kernel/src/socket/af_unix_io.c', name)
code += function('kernel/src/syscalls/sys_fs.c', 'static uint64_t sys_fchdir(')
code += r'''
int main(void) {
 char path[256], next_path[256];
 strcpy(path,"/.wine/dosdevices/c:/windows/system32/");
 for(int i=0;i<1000;i++) {
  assert(fd_make_path(next_path,sizeof(next_path),path,"./././/"));
  strcpy(path,next_path);
 }
 assert(!strcmp(path,"/.wine/dosdevices/c:/windows/system32"));
 assert(fd_make_path(path,sizeof(path),"/ignored","//usr//lib/./wine/"));
 assert(!strcmp(path,"/usr/lib/wine"));
 assert(fd_make_path(path,sizeof(path),"/symlink","../file"));
 assert(!strcmp(path,"/symlink/../file"));
 assert(!fd_make_path(path,4,"/abcd","file"));
 struct fd_table ta={0},tb={0};
 struct thread a={.files=&ta}, b={.files=&tb};
 vfs_node_t dir={.flags=FS_DIRECTORY,.refs=1};
 a.fds[3]=&dir; a.fd_offsets[3]=123;
 a.fd_flags[3]=O_NONBLOCK|FD_FLAGS_CLOEXEC_BIT;
 strcpy(a.paths[3],"/.wine/drive_c/windows/system32");
 unix_scm_fd_t saved;
 assert(unix_scm_capture_fd(&a,3,&saved));
 assert(dir.refs==2 && saved.flags==O_NONBLOCK && saved.offset==123);
 /* Sender may close immediately: queued reference and path survive. */
 a.fds[3]=NULL; a.paths[3][0]=0; vfs_close(&dir);
 int fd=unix_scm_install_fd(&b,&saved,MSG_CMSG_CLOEXEC);
 assert(fd==0 && b.fds[fd]==&dir && dir.refs==1);
 assert(b.fd_flags[fd]==(O_NONBLOCK|FD_FLAGS_CLOEXEC_BIT));
 assert(b.fd_offsets[fd]==123 && !strcmp(b.paths[fd],saved.path));
 current=&b;
 assert(sys_fchdir(fd,0,0,0,0,0)==0 && b.cwd_node==&dir);
 assert(!strcmp(b.cwd_path,"/.wine/drive_c/windows/system32"));
 assert(!unix_scm_capture_fd(&a,3,&saved));
 a.fds[3]=FD_RESERVED;
 assert(!unix_scm_capture_fd(&a,3,&saved));
 /* CLOEXEC comes from recv flags, not the sender's descriptor bit. */
 saved.node=&dir; saved.flags=O_NONBLOCK; saved.path[0]=0;
 vfs_node_ref(&dir);
 fd=unix_scm_install_fd(&b,&saved,0);
 assert(fd==1 && b.fd_flags[fd]==O_NONBLOCK && !b.paths[fd][0]);
 /* Path allocation failure rolls back reservation and free-slot hint. */
 unsigned before=tb.next_fd;
 path_oom=true;
 assert(unix_scm_install_fd(&b,&saved,0)==-12 && tb.next_fd==before && !b.fds[before]);
 path_oom=false;
 /* High minimum allocation must leave lower unused slots available. */
 memset(&a,0,sizeof(a)); memset(&ta,0,sizeof(ta)); a.files=&ta;
 assert(alloc_fd_from(&a,MAX_FDS-1)==MAX_FDS-1 && ta.next_fd==0);
 assert(alloc_fd(&a)==0);
 fd_release_reserved(&a,0);
 assert(alloc_fd(&a)==0);
 for(int i=1;i<MAX_FDS-1;i++) assert(alloc_fd(&a)==i);
 assert(alloc_fd(&a)==-1);
 /* pipe2 reserves its last slot, then fails to reserve a second. */
 fd_release_reserved(&a,MAX_FDS-1);
 assert(alloc_fd(&a)==MAX_FDS-1 && alloc_fd(&a)==-1);
 fd_release_reserved(&a,MAX_FDS-1);
 assert(alloc_fd(&a)==MAX_FDS-1);
 /* A stale exhausted hint must not hide released lower slots. */
 a.fds[4]=NULL; ta.next_fd=MAX_FDS;
 assert(alloc_fd(&a)==4 && ta.next_fd==5);
 a.fds[4]=NULL; ta.next_fd=MAX_FDS;
 assert(alloc_fd_from(&a,4)==4 && ta.next_fd==0);
 assert(alloc_fd(&a)==-1);
 /* Cancel must never close an already published descriptor. */
 a.fds[0]=&dir; fd_release_reserved(&a,0); assert(a.fds[0]==&dir);
 unix_scm_msg_t *m=calloc(1,sizeof(*m)+sizeof(unix_scm_fd_t));
 assert(m); m->count=1; m->stream_offset=64; m->fds[0].node=&dir;
 vfs_node_ref(&dir); int refs=dir.refs;
 unix_sock_t sock={.bytes_read=64,.scm_queue_head=m,.scm_queue_tail=m};
 unix_discard_consumed_scm(&sock);
 assert(sock.scm_queue_head==m && dir.refs==refs);
 sock.bytes_read++;
 unix_discard_consumed_scm(&sock);
 assert(!sock.scm_queue_head && !sock.scm_queue_tail && dir.refs==refs-1);
 puts("PASS: SCM directory fchdir, paths/status/CLOEXEC, sender close, reserved sources, allocation rollback, stream SCM boundary");
}
'''
with tempfile.TemporaryDirectory(prefix='avory-wine-fd-') as tmp:
    p = Path(tmp)
    (p / 'test.c').write_text(code)
    subprocess.run(['cc', '-Wall', '-Wextra', '-Werror', '-fsanitize=undefined,address', str(p / 'test.c'), '-o', str(p / 'test')], check=True)
    subprocess.run([str(p / 'test')], check=True,
                   env={**os.environ, 'ASAN_OPTIONS': 'detect_leaks=0'})
