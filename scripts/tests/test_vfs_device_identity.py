#!/usr/bin/env python3
"""Exercise native mount and pseudo-inode identity used by libudev."""
from pathlib import Path
import subprocess
import tempfile
import re

ROOT = Path(__file__).resolve().parents[2]


def function(path, name):
    source = (ROOT / path).read_text()
    match = re.search(r"^[A-Za-z_][^\n]*\b" + name + r"\([^;{}]*\)\s*\{", source, re.M)
    assert match, name
    start = match.start()
    brace = source.index("{", start)
    depth = 1
    end = brace + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


vfs = "kernel/src/fs/vfs.c"
code = r'''
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#define FS_DIRECTORY 2
#define FS_FILE 1
#define FS_CHARDEV 3
#define FS_SYMLINK 6
#define FS_TYPE_MASK 15
#define FS_PERSISTENT 256
#define FS_MOUNTPOINT 512
typedef struct vfs_node {
 uint64_t mount_id;
 uint32_t inode, flags;
 char name[128];
 void *device;
 struct vfs_node *ptr;
 int (*statfs)(struct vfs_node *,void *);
 struct vfs_node *(*finddir)(struct vfs_node *,char *);
} vfs_node_t;
typedef struct vfs_mount_entry {
 uint64_t mount_id;
 vfs_node_t *mountpoint,*target;
 char dev_name[64],fs_type[32],path[256];
 struct vfs_mount_entry *next;
} vfs_mount_entry_t;
typedef struct { uint64_t mount_id; char mountpoint[128],target[128],dev_name[64],fs_type[32]; } vfs_mount_info_t;
static vfs_mount_entry_t *vfs_mount_list;
static uint64_t vfs_next_mount_id=18;
static uint32_t next_inode=100;
static vfs_node_t *fs_root,*child;
static void *kmalloc(size_t n){return calloc(1,n);}
static void vfs_dentry_invalidate(vfs_node_t *n,const char *s){(void)n;(void)s;}
static vfs_node_t *vfs_dentry_lookup(vfs_node_t *n,char *s,bool *neg){(void)n;(void)s;*neg=false;return NULL;}
static void vfs_dentry_insert(vfs_node_t *n,char *s,vfs_node_t *c){(void)n;(void)s;(void)c;}
static void vfs_close(vfs_node_t *n){(void)n;}
static void vfs_open(vfs_node_t *n){(void)n;}
static vfs_node_t *lookup(vfs_node_t *n,char *s){(void)n;(void)s;return child;}
static void *vfs_dirindex_finddir(vfs_node_t *n,char *s){(void)n;(void)s;return NULL;}
static void ramfs_add_child(vfs_node_t *n,vfs_node_t *c){(void)n;(void)c;}
static int ramfs_statfs(vfs_node_t *n,void *b){(void)n;(void)b;return 0;}
static int sysfs_statfs(vfs_node_t *n,void *b){(void)n;(void)b;return 1;}
#define AT_FDCWD -100
#define MAX_FDS 8
struct thread {vfs_node_t *fds[MAX_FDS];};
static struct thread task;
static struct thread *sched_get_current(void){return &task;}
static bool fd_is_valid_node(vfs_node_t *n){return n!=NULL;}
static void vfs_node_ref(vfs_node_t *n){(void)n;}
static vfs_node_t *vfs_resolve_path_at(vfs_node_t *base,const char *path){
 (void)path;return base;
}
static int vfs_readlink(vfs_node_t *n,char *buf,uint32_t size){
 (void)n;const char *s="../drm/card0";size_t len=strlen(s);if(len>size)len=size;
 memcpy(buf,s,len);return (int)len;
}
static uint64_t sys_readlink(uint64_t a,uint64_t b,uint64_t c,uint64_t d,uint64_t e,uint64_t f){
 (void)a;(void)b;(void)c;(void)d;(void)e;(void)f;return (uint64_t)-2;
}
'''
abi = (ROOT / "kernel/src/syscalls/sys_io_shared.h").read_text()
code += abi[abi.index("struct statx_timestamp {"):abi.index("#define STATX_TYPE")]
code += '\n_Static_assert(sizeof(struct statx)==256,"statx ABI size");\n'
code += '_Static_assert(offsetof(struct statx,stx_mnt_id)==144,"statx mount ID offset");\n'
code += function(vfs, "vfs_finddir")
code += function(vfs, "vfs_mount_ex")
code += function(vfs, "vfs_get_mounts")
code += function("kernel/src/fs/ramfs.c", "ramfs_mount_node")
code += function("kernel/src/syscalls/sys_fs.c", "vfs_resolve_symlink_node")
code += function("kernel/src/syscalls/sys_fs.c", "do_sys_readlink_at")
code += function("kernel/src/syscalls/sys_fs.c", "sys_readlinkat")
code += r'''
int main(void){
 vfs_node_t root={.inode=1}, sys={.inode=2,.flags=FS_DIRECTORY,.finddir=lookup,.statfs=sysfs_statfs};
 vfs_node_t tmp={.inode=3}, a={.flags=FS_DIRECTORY}, b={.flags=FS_DIRECTORY};
 vfs_node_t preset={.flags=FS_FILE,.inode=777}, device={.flags=FS_CHARDEV};
 fs_root=&root;
 assert(vfs_mount_ex(NULL,&sys,"sysfs","sysfs","/sys")==0);
 uint64_t sys_id=sys.mount_id;
 ramfs_mount_node(&sys,&a);ramfs_mount_node(&sys,&b);
 assert(a.inode && b.inode && a.inode!=b.inode);
 assert(a.statfs==sysfs_statfs);
 ramfs_mount_node(&sys,&preset);ramfs_mount_node(&sys,&device);
 assert(preset.inode==777 && device.inode==0);
 child=&a;assert(vfs_finddir(&sys,"a")->mount_id==sys_id);
 child=&b;assert(vfs_finddir(&sys,"b")->mount_id==sys_id);
 assert(vfs_mount_ex(NULL,&tmp,"tmpfs","tmpfs","/tmp")==0);
 assert(tmp.mount_id && tmp.mount_id!=sys_id);
 vfs_mount_info_t mounts[4];int n=vfs_get_mounts(mounts,4);
 bool found=false;for(int i=0;i<n;i++)if(!strcmp(mounts[i].mountpoint,"/sys")){
   assert(mounts[i].mount_id==sys_id);found=true;
 }assert(found);
 assert(vfs_resolve_symlink_node(&sys,".")==&sys);
 vfs_node_t link={.flags=FS_SYMLINK};task.fds[1]=&link;
 char buf[32]={0};
 assert((int64_t)sys_readlinkat(1,(uintptr_t)"",(uintptr_t)buf,sizeof(buf),0,0)==12);
 assert(!strcmp(buf,"../drm/card0"));
 task.fds[1]=&sys;
 assert((int64_t)sys_readlinkat(1,(uintptr_t)"",(uintptr_t)buf,sizeof(buf),0,0)==-22);
 return 0;
}
'''
with tempfile.TemporaryDirectory(prefix="avory-device-identity-") as directory:
    source = Path(directory) / "test.c"
    binary = Path(directory) / "test"
    source.write_text(code)
    subprocess.run(["cc", "-Wall", "-Wextra", "-Werror", str(source), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
print("PASS: pseudo-inode/mount identity, dot resolution, empty-path readlinkat; device numbers preserved")
