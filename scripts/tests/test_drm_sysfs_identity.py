#!/usr/bin/env python3
"""Execute the registered DRM minor bridge against a bounded sysfs tree."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'kernel/src/fs/sysfs.c').read_text()
start = source.index('static vfs_node_t *sysfs_drm_minor_parent(')
end = source.index('static bool sysfs_child_exists(', start)
fixture = r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
typedef struct node { char path[256], value[256]; int alive; } vfs_node_t;
static vfs_node_t nodes[128];
static unsigned count;
static vfs_node_t *asc_sysfs_dir_by_path(const char *path) {
  for (unsigned index=0; index<count; index++)
    if (nodes[index].alive && !strcmp(nodes[index].path,path)) return &nodes[index];
  return NULL;
}
static vfs_node_t *make(const char *path) {
  vfs_node_t *node=asc_sysfs_dir_by_path(path);
  if (node) return node;
  assert(count<128); node=&nodes[count++];
  strcpy(node->path,path); node->alive=1; return node;
}
static vfs_node_t *sysfs_mkdir(vfs_node_t *parent,const char *name) {
  char path[256]; if (!parent) return NULL;
  assert(strlen(parent->path)+strlen(name)+2<sizeof(path));
  strcpy(path,parent->path); strcat(path,"/"); strcat(path,name); return make(path);
}
static void sysfs_mkfile(vfs_node_t *parent,const char *name,const char *value) {
  vfs_node_t *node=sysfs_mkdir(parent,name); assert(node); strcpy(node->value,value);
}
static void sysfs_symlink(vfs_node_t *parent,const char *name,const char *value) {
  sysfs_mkfile(parent,name,value);
}
static void asc_sysfs_remove(void *opaque,const char *name) {
  vfs_node_t *parent=opaque; char path[256]; if (!parent) return;
  strcpy(path,parent->path); strcat(path,"/"); strcat(path,name);
  for (unsigned index=0;index<count;index++)
    if (!strcmp(nodes[index].path,path) ||
        (!strncmp(nodes[index].path,path,strlen(path)) && nodes[index].path[strlen(path)]=='/'))
      nodes[index].alive=0;
}
'''
checks = r'''
int main(void) {
  vfs_node_t *class_dir=make("/sys/class/drm");
  make("/sys/dev/char"); make("/sys/devices"); make("/sys/bus");
  make("/sys/bus/pci/devices/0000:01:00.0");
  make("/sys/bus/pci/devices/0000:03:00.0");
  vfs_node_t *render=asc_sysfs_drm_minor_add(class_dir,"renderD131","0000:01:00.0",NULL,NULL,226,131);
  assert(render);
  assert(!strcmp(asc_sysfs_dir_by_path("/sys/class/drm/renderD131")->value,
                 "/sys/bus/pci/devices/0000:01:00.0/drm/renderD131"));
  assert(!strcmp(asc_sysfs_dir_by_path("/sys/bus/pci/devices/0000:01:00.0/drm/renderD131/device")->value,
                 "/sys/bus/pci/devices/0000:01:00.0"));
  assert(asc_sysfs_drm_minor_add(class_dir,"card7","0000:01:00.0",NULL,NULL,226,7));
  assert(asc_sysfs_drm_minor_add(class_dir,"card2","0000:03:00.0",NULL,NULL,226,2));
  assert(asc_sysfs_drm_minor_add(class_dir,"renderD128",NULL,"faux","vgem",226,128));
  assert(asc_sysfs_dir_by_path("/sys/devices/faux/vgem/drm/renderD128/device"));
  assert(!strcmp(asc_sysfs_dir_by_path("/sys/dev/char/226:128")->value,
                 "/sys/devices/faux/vgem/drm/renderD128"));
  assert(!asc_sysfs_drm_minor_add(class_dir,"card9","0000:09:00.0",NULL,NULL,226,9));
  asc_sysfs_drm_minor_remove(class_dir,"renderD131","0000:01:00.0",NULL,NULL,226,131);
  assert(!asc_sysfs_dir_by_path("/sys/class/drm/renderD131"));
  assert(!asc_sysfs_dir_by_path("/sys/class/drm/devices/renderD131"));
  assert(!asc_sysfs_dir_by_path("/sys/dev/char/226:131"));
  assert(!asc_sysfs_dir_by_path("/sys/bus/pci/devices/0000:01:00.0/drm/renderD131"));
  assert(asc_sysfs_dir_by_path("/sys/bus/pci/devices/0000:01:00.0/drm/card7"));
  assert(asc_sysfs_dir_by_path("/sys/bus/pci/devices/0000:03:00.0/drm/card2"));
  assert(asc_sysfs_drm_minor_add(class_dir,"renderD131","0000:03:00.0",NULL,NULL,226,131));
  assert(!strcmp(asc_sysfs_dir_by_path("/sys/dev/char/226:131")->value,
                 "/sys/bus/pci/devices/0000:03:00.0/drm/renderD131"));
  make("/sys/bus/pci/devices/0000:00:06.0");
  assert(asc_sysfs_drm_minor_add(class_dir,"renderD137","0000:00:06.0",NULL,NULL,226,137));
  assert(!strcmp(asc_sysfs_dir_by_path("/sys/dev/char/226:137")->value,
                 "/sys/bus/pci/devices/0000:00:06.0/drm/renderD137"));
  assert(!strcmp(asc_sysfs_dir_by_path("/sys/bus/pci/devices/0000:00:06.0/drm/renderD137/device")->value,
                 "/sys/bus/pci/devices/0000:00:06.0"));
  puts("PASS: actual PCI parent, shuffled minors, virtual identity, removal and reassignment");
}
'''
with tempfile.TemporaryDirectory(prefix='avory-drm-sysfs-') as temp:
    path = Path(temp)
    (path / 'test.c').write_text(fixture + source[start:end] + checks)
    subprocess.run(['cc', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
                    str(path / 'test.c'), '-o', str(path / 'test')], check=True)
    subprocess.run([str(path / 'test')], check=True)
