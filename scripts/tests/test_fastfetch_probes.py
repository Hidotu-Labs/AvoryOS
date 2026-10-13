#!/usr/bin/env python3
"""Run the kernel's interface ioctl and DRM path walkers with host mocks."""
from pathlib import Path
import os
import subprocess
import tempfile


def extract(path, signature):
    source = Path(path).read_text()
    start = source.index(signature)
    end = source.index('{', start) + 1
    depth = 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end] + '\n'


common = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
'''
socket_code = common + r'''
struct avory_ifreq64 {char name[16];uint16_t family;uint8_t address_data[14],padding[8];};
_Static_assert(sizeof(struct avory_ifreq64)==40,"ifreq ABI");
struct net_device {char name[16];bool registered;};
struct ipv4_config {uint32_t address;};
static struct net_device device={"eth0",true};
static struct ipv4_config config={0x0a00020f};
static bool have_device=true,bad_read,bad_write;
static struct net_device *net_device_default(void){return have_device?&device:NULL;}
static const struct ipv4_config *ipv4_get_config(void){return &config;}
static bool vmm_is_user_addr_range_valid(uint64_t a,size_t n){(void)n;return a>=4096;}
static int copy_from_user(void *to,const void *from,size_t n){if(bad_read)return 1;memcpy(to,from,n);return 0;}
static int copy_to_user(void *to,const void *from,size_t n){if(bad_write)return 1;memcpy(to,from,n);return 0;}
'''
for signature in ('static bool socket_ifreq_name_is(', 'static int socket_ioctl_ifreq('):
    socket_code += extract('kernel/src/socket/socket.c', signature)
socket_code += r'''
int main(void){
 struct avory_ifreq64 ifr;uint64_t arg=(uintptr_t)&ifr;
 for(int index=1;index<=2;index++){
  memset(&ifr,0xa5,sizeof(ifr));memcpy(&ifr.family,&index,4);
  unsigned char saved[24];memcpy(saved,&ifr.family,24);
  assert(!socket_ioctl_ifreq(0x8910,arg));
  assert(!strcmp(ifr.name,index==1?"lo":"eth0"));
  assert(!memcmp(saved,&ifr.family,24));
  assert(!socket_ioctl_ifreq(0x8933,arg));
  int result;memcpy(&result,&ifr.family,4);assert(result==index);
  assert(!socket_ioctl_ifreq(0x8915,arg));assert(ifr.family==2);
  assert(ifr.address_data[2]==(index==1?127:10));
 }
 int invalid[]={-1,0,3,2147483647};
 for(unsigned i=0;i<sizeof(invalid)/sizeof(*invalid);i++){
  memcpy(&ifr.family,&invalid[i],4);assert(socket_ioctl_ifreq(0x8910,arg)==-19);
 }
 int index=2;memcpy(&ifr.family,&index,4);device.registered=false;
 assert(socket_ioctl_ifreq(0x8910,arg)==-19);device.registered=true;
 have_device=false;assert(socket_ioctl_ifreq(0x8910,arg)==-19);
 index=1;memcpy(&ifr.family,&index,4);assert(!socket_ioctl_ifreq(0x8910,arg));
 assert(!socket_ioctl_ifreq(0x8933,arg));have_device=true;
 strcpy(ifr.name,"missing");assert(socket_ioctl_ifreq(0x8933,arg)==-19);
 assert(socket_ioctl_ifreq(0x8910,0)==-14);assert(socket_ioctl_ifreq(0x8910,1)==-14);
 index=2;memcpy(&ifr.family,&index,4);
 bad_read=true;assert(socket_ioctl_ifreq(0x8910,arg)==-14);bad_read=false;
 bad_write=true;assert(socket_ioctl_ifreq(0x8910,arg)==-14);bad_write=false;
 puts("PASS: interface name/index round trips, loopback, missing devices, invalid indexes and usercopy faults");
}
'''
vfs_code = common + r'''
#define FS_TYPE_MASK 7
#define FS_FILE 1
#define FS_DIRECTORY 2
#define FS_SYMLINK 3
#define FS_DENTRY_NOCACHE 256
#define MAX_SYMLINK_DEPTH 8
struct node {unsigned flags;int refs;const char *name;struct node *parent;};
typedef struct node vfs_node_t;
static struct node nodes[]={
 {FS_DIRECTORY,0,"",NULL},{FS_DIRECTORY,0,"sys",&nodes[0]},
 {FS_DIRECTORY,0,"class",&nodes[1]},{FS_DIRECTORY,0,"drm",&nodes[2]},
 {FS_FILE,0,"version",&nodes[3]},{FS_DIRECTORY,0,"card0-HDMI-A-1",&nodes[3]},
 {FS_FILE,0,"connector_id",&nodes[5]}};
static vfs_node_t *fs_root=&nodes[0];
typedef struct mount {vfs_node_t *mountpoint,*target;struct mount *next;} vfs_mount_entry_t;
static vfs_mount_entry_t *vfs_mount_list;
static bool cache_enabled;static int file_searches;
static void vfs_open(vfs_node_t *n){n->refs++;}
static void vfs_close(vfs_node_t *n){assert(n->refs>0);n->refs--;}
static bool vfs_node_is_alive(vfs_node_t *n){(void)n;return true;}
static bool vfs_node_tryref(vfs_node_t *n){vfs_open(n);return true;}
static bool vfs_access(vfs_node_t *n,unsigned mode){(void)mode;if(n->flags==FS_FILE){file_searches++;return false;}return true;}
static bool vfs_path_has_dotdot(const char *p){return strstr(p,"..")!=NULL;}
static vfs_node_t *vfs_path_cache_lookup(vfs_node_t *n,const char *p){(void)n;(void)p;return NULL;}
static void vfs_path_cache_insert(vfs_node_t *n,const char *p,vfs_node_t *v){(void)n;(void)p;(void)v;}
static vfs_node_t *vfs_finddir(vfs_node_t *n,char *name){
 assert(n->flags==FS_DIRECTORY);
 for(unsigned i=1;i<sizeof(nodes)/sizeof(*nodes);i++)if(nodes[i].parent==n && !strcmp(name,nodes[i].name)){vfs_open(&nodes[i]);return &nodes[i];}
 return NULL;
}
static vfs_node_t *vfs_dentry_lookup(vfs_node_t *n,char *name,bool *negative){*negative=false;return cache_enabled?vfs_finddir(n,name):NULL;}
static int vfs_readlink(vfs_node_t *n,char *p,int size){(void)n;(void)p;(void)size;return -1;}
'''
walker = extract('kernel/src/fs/vfs.c', 'static vfs_node_t *vfs_lookup_cached_path(')
# Use the real VFS node ABI fields even though no mount is installed.
vfs_code = vfs_code.replace('struct node *parent;}', 'struct node *parent;unsigned inode;void *device;}')
vfs_code += walker + extract('kernel/src/fs/vfs.c', 'vfs_node_t *vfs_resolve_path_at(')
vfs_code += r'''
int main(void){
 for(unsigned cached=0;cached<2;cached++){
  cache_enabled=cached;
  const char *invalid[]={"/sys/class/drm/version/connector_id","/sys/class/drm/version/.","/sys/class/drm/version/.."};
  for(unsigned i=0;i<3;i++)assert(!vfs_resolve_path_at(fs_root,invalid[i]));
  assert(!file_searches);
  vfs_node_t *n=vfs_resolve_path_at(fs_root,"/sys/class/drm/version");assert(n==&nodes[4]);vfs_close(n);
  n=vfs_resolve_path_at(fs_root,"/sys/class/drm/card0-HDMI-A-1/connector_id");assert(n==&nodes[6]);vfs_close(n);
  assert(!vfs_resolve_path_at(fs_root,"/sys/class/drm/missing"));
  for(unsigned i=0;i<sizeof(nodes)/sizeof(*nodes);i++)assert(!nodes[i].refs);
 }
 puts("PASS: cached/general DRM walkers reject file traversal without a missing-node diagnostic or leaked references");
}
'''
with tempfile.TemporaryDirectory() as directory:
    tmp = Path(directory)
    for name, code in [('socket', socket_code), ('vfs', vfs_code)]:
        source = tmp / f'{name}.c'
        binary = tmp / name
        source.write_text(code)
        subprocess.run(['cc', '-O1', '-g', '-fsanitize=address,undefined', str(source), '-o', str(binary)], check=True)
        subprocess.run([str(binary)], check=True, env={**os.environ, 'ASAN_OPTIONS': 'detect_leaks=0'})
