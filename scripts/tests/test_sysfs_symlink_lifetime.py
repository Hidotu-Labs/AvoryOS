#!/usr/bin/env python3
"""Dynamic sysfs symlinks must release their target and lookup references."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'kernel/src/fs/sysfs.c').read_text()
implementation = source[source.index('static int sysfs_readlink_cb('):source.index('/* uevent attributes are commands')]
fixture = r'''
#include <stdint.h>
#include <assert.h>
#include <stdlib.h>
#include <string.h>
typedef struct vfs_node {
  char name[128]; unsigned flags,mask;
  struct vfs_node *ptr;
  int (*readlink)(struct vfs_node *,char *,uint32_t);
  void (*destroy)(struct vfs_node *);
} vfs_node_t;
#define FS_SYMLINK 3
#define FS_PERSISTENT 8
static vfs_node_t *attached;
static unsigned allocations, references;
static int fail_target;
static void *kmalloc(size_t size) {
  if (fail_target && allocations==1) return NULL;
  void *pointer=malloc(size); if (pointer) allocations++; return pointer;
}
static void kfree(void *pointer) { if(pointer) { assert(allocations); allocations--; free(pointer); } }
static void vfs_node_init(vfs_node_t *node) { memset(node,0,sizeof(*node)); }
static vfs_node_t *vfs_finddir(vfs_node_t *parent,char *name) {
  (void)parent;
  if (attached && !strcmp(attached->name,name)) { references++; return attached; }
  return NULL;
}
static void vfs_close(vfs_node_t *node) { assert(node==attached && references); references--; }
static void ramfs_mount_node(vfs_node_t *parent,vfs_node_t *node) { assert(parent && !attached); attached=node; }
static void vfs_dentry_invalidate(vfs_node_t *parent,const char *name) { (void)parent; (void)name; }
'''
checks = r'''
int main(void) {
  vfs_node_t parent={0}; char target[128];
  for(unsigned iteration=0;iteration<100;iteration++) {
    sysfs_symlink(&parent,"renderD129","/sys/bus/pci/devices/0000:01:00.0/drm/renderD129");
    assert(attached && allocations==2 && attached->destroy);
    assert(attached->readlink(attached,target,sizeof(target))==
           (int)strlen("/sys/bus/pci/devices/0000:01:00.0/drm/renderD129"));
    sysfs_symlink(&parent,"renderD129","ignored");
    assert(allocations==2 && !references);
    attached->destroy(attached); assert(!attached->ptr);
    kfree(attached); attached=NULL; assert(!allocations);
  }
  fail_target=1;
  sysfs_symlink(&parent,"renderD129","target");
  assert(!attached && !allocations && !references);
}
'''
with tempfile.TemporaryDirectory(prefix='avory-sysfs-link-') as temp:
    path = Path(temp)
    (path / 'test.c').write_text(fixture + implementation + checks)
    subprocess.run(['cc', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
                    str(path / 'test.c'), '-o', str(path / 'test')], check=True)
    subprocess.run([str(path / 'test')], check=True)
print('PASS: repeated symlink teardown, duplicate lookup reference and target-allocation failure')
