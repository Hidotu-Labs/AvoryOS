#!/usr/bin/env python3
"""Exercise built-in char-device registration and stat APIs against Wine's gate."""
from pathlib import Path
import subprocess
import tempfile
root = Path(__file__).resolve().parents[2]
def extract(file, signature):
    source = (root / file).read_text()
    start = source.index(signature)
    end = source.index('{', start) + 1
    depth = 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end] + (';\n' if signature.startswith('struct ') else '\n')
code = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define FS_TYPE_MASK 7
#define FS_FILE 1
#define FS_DIRECTORY 2
#define FS_CHARDEV 3
#define FS_BLOCKDEV 4
#define FS_SYMLINK 6
#define FS_SOCKET 7
#define FS_PERSISTENT 0x100
#define AT_FDCWD -100
#define AT_EMPTY_PATH 0x1000
#define AT_SYMLINK_NOFOLLOW 0x100
#define STATX_BASIC_STATS 0x7ff
#define MAX_FDS 4
#define PTY_DEBUG_LOGGING 0
struct statfs_buf;
typedef struct vfs_node {
 char name[128]; uint32_t flags, inode, mask, uid, gid;
 uint64_t length, atime, mtime, ctime;
 void (*read)(void), (*write)(void), (*poll)(void), (*mmap)(void),
      (*ioctl)(void), (*open)(void), (*close)(void);
 void *device, *wait_queue;
 int (*statfs)(struct vfs_node *, struct statfs_buf *);
} vfs_node_t;
struct block_device {uint32_t linux_major, linux_minor;};
static uint32_t block_device_userspace_rdev(struct block_device *b){return b->linux_major<<8|b->linux_minor;}
static int ext4_statfs_impl(vfs_node_t *n,struct statfs_buf *s){(void)n;(void)s;return 0;}
static int ext2_statfs_impl(vfs_node_t *n,struct statfs_buf *s){(void)n;(void)s;return 0;}
static int fat32_statfs_impl(vfs_node_t *n,struct statfs_buf *s){(void)n;(void)s;return 0;}
static vfs_node_t console_vfs_node,tty0_vfs_node,tty_vfs_node,fb_vfs_node,fb_alias_node;
static struct {vfs_node_t *vfs_node;uint64_t screen_size;} fb_global;
static int keyboard_wait_queue, fb_registry_lock;
static struct {char name[128];vfs_node_t *node;} fb_device_registry[32];
static size_t fb_device_count;
static void *kmalloc(size_t n){return calloc(1,n);}
static void vfs_node_init(vfs_node_t *n){memset(n,0,sizeof(*n));}
static void devfs_register_node(const char *name,vfs_node_t *n){(void)name;(void)n;}
static vfs_node_t *devfs_lookup(const char *name){(void)name;return NULL;}
static void fb_register_device_node(const char *name,vfs_node_t *n){
 strcpy(fb_device_registry[fb_device_count].name,name);
 fb_device_registry[fb_device_count++].node=n;
}
static void spinlock_acquire(int *l){(void)l;}
static void spinlock_release(int *l){(void)l;}
static void klog_puts(const char *s){(void)s;}
#define STUB(name) static void name(void){}
STUB(null_read) STUB(null_write) STUB(null_poll) STUB(zero_read)
STUB(zero_write) STUB(zero_poll)
STUB(zero_mmap) STUB(random_read) STUB(ptmx_read) STUB(ptmx_write)
STUB(ptmx_ioctl) STUB(ptmx_poll) STUB(pty_init)
STUB(fb_dev_read) STUB(fb_dev_write) STUB(fb_dev_mmap) STUB(fb_dev_ioctl)
STUB(fb_dev_open) STUB(fb_dev_close) STUB(fb_dev_poll)
STUB(console_dev_read) STUB(console_dev_write) STUB(console_dev_ioctl)
STUB(console_dev_open) STUB(console_dev_close) STUB(console_dev_poll)
struct thread {vfs_node_t *fds[MAX_FDS];char cwd_path[128];};
static struct thread current;
static vfs_node_t *fs_root=&console_vfs_node;
static struct thread *sched_get_current(void){return &current;}
static bool vmm_is_user_addr_range_valid(uint64_t p,size_t n){(void)n;return p!=0;}
static long strncpy_from_user(char *dst,const char *src,long n){
 size_t len=strlen(src);memcpy(dst,src,len<(size_t)n?len+1:(size_t)n);return (long)len;
}
static int copy_to_user(void *d,const void *s,size_t n){memcpy(d,s,n);return 0;}
static vfs_node_t *vfs_resolve_path_at(vfs_node_t *b,const char *p){(void)b;(void)p;return NULL;}
static vfs_node_t *vfs_resolve_symlink_node(vfs_node_t *b,const char *p){return vfs_resolve_path_at(b,p);}
static void vfs_close(vfs_node_t *n){(void)n;}
'''
for signature in ['struct kstat {', 'struct statx_timestamp {', 'struct statx {']:
    code += extract('kernel/src/syscalls/sys_io_shared.h', signature)
for file, signature in [
    ('kernel/src/fs/random.c','void random_register_vfs('),
    ('kernel/src/drivers/pty.c','void pty_register_devices('),
    ('kernel/src/fb/framebuffer.c','void fb_register_vfs('),
    ('kernel/src/fb/framebuffer.c','vfs_node_t *fb_lookup_device('),
    ('kernel/src/syscalls/sys_stat.c','void fill_kstat('),
    ('kernel/src/syscalls/sys_stat.c','static uint64_t sys_statx('),
]:
    code += extract(file, signature)
code += r'''
static uint64_t number(const char *name){
 vfs_node_t *n=fb_lookup_device(name);assert(n);
 struct kstat s;fill_kstat(&s,n);assert((s.st_mode&0170000)==0020000);
 struct statx x;char path[160];snprintf(path,sizeof(path),"/dev/%s",name);
 assert(!sys_statx(AT_FDCWD,(uint64_t)path,0,0,(uint64_t)&x,0));
 assert(x.stx_rdev_major==(s.st_rdev>>8) && x.stx_rdev_minor==(s.st_rdev&255));
 current.fds[1]=n;
 assert(!sys_statx(1,(uint64_t)"",AT_EMPTY_PATH,0,(uint64_t)&x,0));
 assert(x.stx_rdev_major==(s.st_rdev>>8) && x.stx_rdev_minor==(s.st_rdev&255));
 return s.st_rdev;
}
int main(void){
 random_register_vfs();pty_register_devices();fb_register_vfs();
 uint64_t null=number("null");assert(null==0x103);
 assert(number("zero")==0x105 && number("random")==0x108 && number("urandom")==0x109);
 assert(number("console")==0x501 && number("tty0")==0x400 && number("tty")==0x500);
 assert(number("ptmx")==0x502 && number("fb0")==0x1d00 && number("fb")==0x1d00);
 /* Wine init_options disables logging iff stderr is a character device
    with /dev/null's rdev. Real terminals must fail that comparison. */
 assert(number("console")!=null && number("tty0")!=null && number("tty")!=null);
 assert(number("null")==null); /* stderr deliberately redirected to null */
 for(size_t i=0;i<fb_device_count;i++) {
  if(!strcmp(fb_device_registry[i].name,"null") || !strcmp(fb_device_registry[i].name,"zero") ||
     !strcmp(fb_device_registry[i].name,"random") || !strcmp(fb_device_registry[i].name,"urandom") ||
     !strcmp(fb_device_registry[i].name,"ptmx"))free(fb_device_registry[i].node);
 }
 puts("PASS: registered character devices, path/fd statx, stat agreement and Wine stderr suppression predicate");
}
'''
with tempfile.TemporaryDirectory(prefix='avory-char-dev-') as tmp:
    p = Path(tmp)
    (p/'test.c').write_text(code)
    subprocess.run(['cc','-Wall','-Wextra','-Werror','-fsanitize=undefined',str(p/'test.c'),'-o',str(p/'test')],check=True)
    subprocess.run([str(p/'test')],check=True)
