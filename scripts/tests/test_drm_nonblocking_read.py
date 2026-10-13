#!/usr/bin/env python3
"""DRM event draining must return EAGAIN instead of blocking or EOF."""
from pathlib import Path
import re
import subprocess
import tempfile
ROOT = Path(__file__).resolve().parents[2]
def function(path, name):
    source = (ROOT / path).read_text()
    match = re.search(r'^[A-Za-z_][^\n]*\b' + name + r'\([^;{}]*\)\s*\{', source, re.M)
    assert match, name
    brace = source.index('{', match.start())
    depth, end = 1, brace + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[match.start():end]
code = r'''
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdlib.h>
#include <sys/types.h>
#define __user
#define O_NONBLOCK 0x800
#define FS_NONBLOCK 0x20
#define THREAD_BLOCKED 2
#define THREAD_RUNNING 1
struct vfs_node { unsigned flags; };
typedef struct vfs_node vfs_node_t;
struct file;
struct file_operations { ssize_t (*read)(struct file *,char *,unsigned int,long *); };
struct file { unsigned f_flags; struct file_operations *f_op; long f_pos; };
static struct file imported;
static struct file *kpi_node_file(void *n) { (void)n; return &imported; }
'''
for name in ('asc_vfs_node_nonblocking', 'asc_vfs_node_set_nonblocking'):
    code += function('kernel/src/linuxkpi/native_vfs.c', name)
code += function('kernel/linuxkpi/src/file.c', 'linuxkpi_file_read')
code += r'''
struct list_head { int empty; };
struct drm_file { int lock; struct list_head event_queue; int event_wq; };
struct drm_pending_event { struct list_head list; struct { struct { uint32_t length; } base; } event; };
static struct drm_file native;
static struct drm_pending_event pending;
struct thread { int state; };
static struct thread thread;
static int interrupted, waits, released;
typedef struct { struct thread *thread; void *next,*wq,*thread_next,*thread_prev; } wait_queue_entry_t;
static struct drm_file *node_to_file(struct vfs_node *n) { (void)n; return &native; }
static void spinlock_acquire(int *p) { assert(!*p); *p=1; }
static void spinlock_release(int *p) { assert(*p); *p=0; }
static int list_empty(struct list_head *p) { return p->empty; }
static struct thread *sched_get_current(void) { return &thread; }
static int thread_has_pending_signal(struct thread *t) { (void)t;return interrupted; }
static void wait_queue_add(int *q,wait_queue_entry_t *e) { (void)q;(void)e; waits++; }
static void wait_queue_remove(int *q,wait_queue_entry_t *e) { (void)q;(void)e; }
static void sched_yield(void) { native.event_queue.empty=0; }
#define list_first_entry(head,type,member) ((void)(head), &pending)
static void list_del(void *p) { (void)p; native.event_queue.empty=1; }
static void kfree(void *p) { (void)p;released++; }
'''
code += function('kernel/src/drivers/gpu/drm/drm.c','drm_read')
code += r'''
static ssize_t device_read(struct file *f,char *b,unsigned int n,long *pos) {
 (void)b;(void)n;(void)pos;
 return (f->f_flags & O_NONBLOCK) ? -11 : -4;
}
int main(void) {
 struct vfs_node n={0}; unsigned char b[32];
 struct file_operations ops={device_read}; imported.f_op=&ops; imported.f_flags=2;
 asc_vfs_node_set_nonblocking(&n,1);
 assert((int32_t)linuxkpi_file_read(&n,0,sizeof(b),b)==-11);
 assert(imported.f_flags==(2|O_NONBLOCK));
 asc_vfs_node_set_nonblocking(&n,0);
 assert((int32_t)linuxkpi_file_read(&n,0,sizeof(b),b)==-4);
 assert(imported.f_flags==2);
 native.event_queue.empty=1; n.flags=FS_NONBLOCK;
 assert((int32_t)drm_read(&n,0,sizeof(b),b)==-11);
 assert(!native.lock && !waits);
 assert(drm_read(&n,0,0,b)==0);
 n.flags=0;interrupted=1;
 assert((int32_t)drm_read(&n,0,sizeof(b),b)==-4);
 assert(!native.lock && !waits);
 interrupted=0;pending.event.base.length=4;
 assert(drm_read(&n,0,sizeof(b),b)==4);
 assert(waits==1 && released==1 && !native.lock);
 n.flags=FS_NONBLOCK; native.event_queue.empty=0;
 assert(drm_read(&n,0,sizeof(b),b)==4);
 assert(waits==1 && released==2);
 assert((int32_t)drm_read(&n,0,sizeof(b),b)==-11);
}
'''
with tempfile.TemporaryDirectory(prefix='avory-drm-read-') as d:
    src, exe = Path(d)/'test.c', Path(d)/'test'
    src.write_text(code)
    subprocess.run(['cc','-std=gnu11','-Wall','-Wextra','-Werror',str(src),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
print('DRM nonblocking event read checks passed')
