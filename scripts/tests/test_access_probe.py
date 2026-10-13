#!/usr/bin/env python3
"""Actual VFS access checks and syscall probe tagging without a guest disk."""
from pathlib import Path
import os, subprocess, tempfile

def extract(path,signature):
 s=Path(path).read_text();a=s.index(signature);b=s.index('{',a)+1;d=1
 while d:
  d+=(s[b]=='{')-(s[b]=='}');b+=1
 return s[a:b]+'\n'
code=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define AT_FDCWD -100
#define AT_SYMLINK_NOFOLLOW 0x100
#define AT_EACCESS 0x200
#define AT_EMPTY_PATH 0x1000
#define MAX_FDS 8
#define FS_TYPE_MASK 7
#define FS_FILE 1
#define FS_DIRECTORY 2
typedef struct {unsigned flags,mask,uid,gid;} vfs_node_t;
struct thread {unsigned euid,fsuid,gid,egid,fsgid,supplementary_group_count,supplementary_groups[8];bool last_access_nonexec_probe;char cwd_path[64];vfs_node_t *fds[MAX_FDS];};
static struct thread self;
static vfs_node_t root={.flags=FS_DIRECTORY,.mask=0755},file={.flags=FS_FILE,.mask=0600};
static vfs_node_t *fs_root=&root;
static unsigned closes;
static bool missing;
static struct thread *sched_get_current(void){return &self;}
static vfs_node_t *vfs_resolve_path_at(vfs_node_t *base,const char *path){(void)base;return !path[0]||missing?NULL:&file;}
static vfs_node_t *vfs_resolve_symlink_node(vfs_node_t *base,const char *path){return vfs_resolve_path_at(base,path);}
static vfs_node_t *fb_lookup_device(char *path){(void)path;return NULL;}
static bool fd_is_valid_node(vfs_node_t *n){return n!=NULL;}
static void vfs_close(vfs_node_t *n){assert(n);closes++;}
'''
code+=extract('kernel/src/fs/vfs.c','bool vfs_in_group(')
code+=extract('kernel/src/fs/vfs.c','bool vfs_access(')
code+=extract('kernel/src/syscalls/sys_fs.c','static uint64_t do_sys_access(')
code+=r'''
int main(void){
 /* Root HOME=/ and a regular, non-executable cache file. */
 assert((int64_t)do_sys_access(AT_FDCWD,"/.cache/ksvg-elements",1,0)==-13);
 assert(self.last_access_nonexec_probe && closes==1);
 assert(do_sys_access(AT_FDCWD,"/.cache/ksvg-elements",4,0)==0 && !self.last_access_nonexec_probe);
 assert(do_sys_access(AT_FDCWD,"/.cache/ksvg-elements",2,0)==0 && !self.last_access_nonexec_probe);
 assert(do_sys_access(AT_FDCWD,"/.cache/ksvg-elements",0,0)==0);
 file.mask=0700;assert(!do_sys_access(AT_FDCWD,"/cache",1,0) && !self.last_access_nonexec_probe);
 file.flags=FS_DIRECTORY;file.mask=0600;assert(!do_sys_access(AT_FDCWD,"/cache",1,0) && !self.last_access_nonexec_probe);
 self.euid=self.fsuid=self.gid=self.egid=self.fsgid=1000;
 assert((int64_t)do_sys_access(AT_FDCWD,"/cache",1,0)==-13 && !self.last_access_nonexec_probe);
 file.flags=FS_FILE;file.mask=0600;assert((int64_t)do_sys_access(AT_FDCWD,"/cache",4,0)==-13 && !self.last_access_nonexec_probe);
 assert((int64_t)do_sys_access(AT_FDCWD,"/cache",2,0)==-13 && !self.last_access_nonexec_probe);
 assert((int64_t)do_sys_access(AT_FDCWD,"/cache",5,0)==-13 && !self.last_access_nonexec_probe);
 file.mask=0700;assert((int64_t)do_sys_access(AT_FDCWD,"/cache",1,0)==-13 && !self.last_access_nonexec_probe);
 file.mask=0600;self.fds[3]=&file;closes=0;
 assert((int64_t)do_sys_access(3,"",1,AT_EMPTY_PATH|AT_EACCESS)==-13 && self.last_access_nonexec_probe && !closes);
 assert((int64_t)do_sys_access(AT_FDCWD,NULL,1,0)==-14 && !self.last_access_nonexec_probe);
 self.last_access_nonexec_probe=true;assert((int64_t)do_sys_access(AT_FDCWD,"/cache",1,0x8000)==-22 && !self.last_access_nonexec_probe);
 self.last_access_nonexec_probe=true;missing=true;assert((int64_t)do_sys_access(AT_FDCWD,"/cache",1,0)==-2 && !self.last_access_nonexec_probe);
 puts("PASS: root/non-root cache execute probes preserve EACCES; R/W, mixed modes, directories, executable files, invalid flags, missing paths and borrowed descriptors retain their semantics");
}
'''
with tempfile.TemporaryDirectory() as tmp:
 p=Path(tmp);(p/'test.c').write_text(code)
 subprocess.run(['cc','-O1','-g','-fsanitize=address,undefined',str(p/'test.c'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True,env={**os.environ,'ASAN_OPTIONS':'detect_leaks=0'})
