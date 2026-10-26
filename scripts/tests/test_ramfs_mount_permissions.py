#!/usr/bin/env python3
"""Mounted ramfs roots must remain searchable by unprivileged services."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'kernel/src/fs/ramfs.c').read_text()
function = source[source.index('void ramfs_mount_at(char *path) {'):]
# This is the final function in ramfs.c.
code = r'''
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
typedef struct vfs_node {
 char name[128]; uint32_t mask;
 void (*mkdir)(struct vfs_node *, char *, uint32_t);
} vfs_node_t;
static vfs_node_t covered, *mounted, *fs_root = &covered;
static int missing, closed;
static void *kmalloc(size_t n) { return malloc(n); }
static void vfs_node_init(vfs_node_t *n) { memset(n, 0, sizeof(*n)); }
static vfs_node_t *vfs_resolve_path(char *p) { (void)p; return missing ? NULL : &covered; }
static void ramfs_mount_on(vfs_node_t *n) { (void)n; }
static void vfs_close(vfs_node_t *n) { assert(n == &covered); closed++; }
static void vfs_mount_ex(vfs_node_t *n, vfs_node_t *target,
 const char *dev, const char *type, const char *path) {
 assert(n == &covered && !strcmp(dev, "ramfs") && !strcmp(type, "ramfs"));
 assert(!strcmp(path, "/dev") || !strcmp(path, "/run")); mounted = target;
}
static void make_directory(vfs_node_t *n, char *name, uint32_t mode) {
 assert(n == &covered && !strcmp(name, "run") && mode == 0755); missing = 0;
}
'''
code += function
code += r'''
int main(void) {
 covered.mask = 0700;
 ramfs_mount_at("/dev");
 assert(mounted && mounted->mask == 0755 && !strcmp(mounted->name, "dev"));
 assert((mounted->mask & 5) == 5); /* non-owner may list and search */
 assert(!(mounted->mask & 2));    /* non-owner may not create devices */
 assert(covered.mask == 0700 && closed == 1); free(mounted);
 missing = 1; covered.mkdir = make_directory;
 ramfs_mount_at("/run");
 assert(mounted->mask == 0755 && !strcmp(mounted->name, "run"));
 assert(closed == 2); free(mounted);
}
'''
with tempfile.TemporaryDirectory(prefix='avory-ramfs-permissions-') as tmp:
    src = Path(tmp) / 'probe.c'
    binary = Path(tmp) / 'probe'
    src.write_text(code)
    subprocess.run(['cc', '-Wall', '-Wextra', '-Werror', str(src), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
print('PASS: existing and newly created ramfs mount roots allow non-owner search without write access')
