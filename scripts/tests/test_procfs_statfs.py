#!/usr/bin/env python3
"""Exercise real procfs constructors and ramfs mounting with host allocation."""
from pathlib import Path
import os
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
source = Path(os.environ.get('PROCFS_SOURCE', ROOT / 'kernel/src/fs/procfs.c')).read_text()
ramfs = (ROOT / 'kernel/src/fs/ramfs.c').read_text()

def extract(text, signature):
    start = text.index(signature)
    pos = text.index('{', start)
    depth, end = 1, pos + 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[start:end]

constructors = [extract(source, signature) for signature in (
    'static vfs_node_t *make_pid_dir(',
    'static void procfs_make_pid_ramfs_ephemeral(',
    'static vfs_node_t *procfs_pid_fd_finddir(')]
mounts = [extract(ramfs, 'void ramfs_mount_node('), extract(ramfs, 'void ramfs_mount_on(')]
real = {'ramfs_statfs', 'procfs_statfs', 'vfs_dirindex_finddir_ref', 'procfs_pid_fd_finddir'}
callbacks = {}
for fragment in constructors + mounts:
    for field, name in re.findall(r'->(\w+)\s*=\s*(\w+);', fragment):
        if name.startswith(('procfs_', 'ramfs_')) and name not in real:
            callbacks[name] = field
prefix = r'''
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#define LOCK_SPINLOCK_H
typedef struct { int unused; } spinlock_t;
static void spinlock_acquire(spinlock_t *s) { (void)s; }
static void spinlock_release(spinlock_t *s) { (void)s; }
#include "fs/ramfs.h"
#define PROC_SUPER_MAGIC 0x9fa0
static void *kmalloc(size_t size) { return calloc(1,size); }
void vfs_node_init(vfs_node_t *node) { memset(node,0,sizeof(*node)); }
void vfs_open(vfs_node_t *node) { (void)node; }
void vfs_dirindex_init(vfs_dirindex_t *d) { memset(d,0,sizeof(*d)); }
vfs_node_t *vfs_dirindex_finddir(vfs_node_t *root, char *name) {
  ramfs_dir_t *d=root->device;
  for (child_node_t *c=d->children;c;c=c->next)
    if (!strcmp(c->node->name,name)) return c->node;
  return NULL;
}
vfs_node_t *vfs_dirindex_finddir_ref(vfs_node_t *root, char *name) {
  return vfs_dirindex_finddir(root,name);
}
static void ramfs_add_child(vfs_node_t *root,vfs_node_t *node) {
  ramfs_dir_t *d=root->device;child_node_t *c=kmalloc(sizeof(*c));
  c->node=node;c->next=d->children;d->children=c;
}
static int ramfs_statfs(vfs_node_t *node,struct statfs_buf *buf) {
  (void)node;buf->f_type=0x858458f6;return 0;
}
static uint32_t str_to_pid(const char *name) { return strtoul(name,NULL,10); }
static int sched_get_fd_path_snapshot(uint32_t pid,uint32_t fd,char *out,size_t size) {
  (void)pid;(void)fd;(void)out;(void)size;return 1;
}
'''
stubs = '\n'.join(f'static __typeof__(((vfs_node_t *)0)->{field}) {name};'
                  for name, field in callbacks.items())
init = extract(source, 'static void procfs_node_init(') if 'static void procfs_node_init(' in source else ''
main = r'''
static unsigned checked;
static void check_node(vfs_node_t *node) {
  struct statfs_buf buf;memset(&buf,0xa5,sizeof(buf));
  assert(node->statfs && node->statfs(node,&buf)==0);
  assert(buf.f_type==PROC_SUPER_MAGIC);
  assert(buf.f_bsize==4096 && buf.f_blocks==0);
  for(unsigned i=0;i<4;i++) assert(buf.f_spare[i]==0);
  checked++;
  if((node->flags & FS_TYPE_MASK)==FS_DIRECTORY && node->device) {
    ramfs_dir_t *d=node->device;
    for(child_node_t *c=d->children;c;c=c->next) check_node(c->node);
  }
}
int main(void) {
  vfs_node_t *pid=make_pid_dir(123);check_node(pid);
  assert(checked>25);
  vfs_node_t *maps=vfs_dirindex_finddir(pid,"maps");assert(maps);check_node(maps);
  vfs_node_t *task=vfs_dirindex_finddir(pid,"task");assert(task);
  vfs_node_t *tid=vfs_dirindex_finddir(task,"123");assert(tid);
  check_node(vfs_dirindex_finddir(tid,"stat"));
  vfs_node_t *fds=vfs_dirindex_finddir(pid,"fd");assert(fds);
  vfs_node_t *fd=procfs_pid_fd_finddir(fds,"0");assert(fd);check_node(fd);
  printf("PASS: %u process directories/files/links report procfs, including maps, task stat and dynamic fd links\n",checked);
}
'''
code = prefix + '\nstatic vfs_node_t *procfs_pid_fd_finddir(vfs_node_t *, char *);\n' + stubs + extract(source, 'static int procfs_statfs(') + init
code += ''.join(mounts) + constructors[1] + constructors[0] + constructors[2] + main
with tempfile.TemporaryDirectory() as tmp:
    p=Path(tmp);(p/'test.c').write_text(code)
    subprocess.run(['cc','-O1','-g','-fsanitize=address,undefined',
                    '-I',str(ROOT/'kernel/src'),str(p/'test.c'),'-o',str(p/'test')],check=True)
    subprocess.run([str(p/'test')],check=True,env={**os.environ,'ASAN_OPTIONS':'detect_leaks=0'})
