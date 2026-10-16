#!/usr/bin/env python3
"""Exercise real sysfs uevent writes, preserving the read-only device metadata."""
from pathlib import Path
import subprocess
import tempfile

source = (Path(__file__).resolve().parents[2] / 'kernel/src/fs/sysfs.c').read_text()
start = source.index('static vfs_node_t *uevent_root;')
end = source.index('// Create a read-only file under parent', start)
fixture = r'''
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <assert.h>
#define FS_TYPE_MASK 7
#define FS_DIRECTORY 2
#define NETLINK_KOBJECT_UEVENT 15
struct vfs_node;
typedef struct vfs_node {
 char name[128];unsigned flags,mask;struct vfs_node *ptr;
 int (*readlink)(struct vfs_node*,char*,uint32_t);
 uint32_t (*write)(struct vfs_node*,uint32_t,uint32_t,uint8_t*);
} vfs_node_t;
struct dirent {char name[128];};
static vfs_node_t nodes[7];
static int parents[]={-1,0,1,2,3,4,4};
static char metadata[]="MAJOR=116\nMINOR=0\nDEVNAME=snd/controlC0\nSUBSYSTEM=sound\n";
static char received[4096];static size_t received_len;static uint64_t seq;
static struct dirent *vfs_readdir(vfs_node_t *dir,uint32_t index){
 static struct dirent entry;
 for(int i=0;i<7;i++)if(parents[i]==dir-nodes){if(index--==0){strcpy(entry.name,nodes[i].name);return &entry;}}
 return NULL;
}
static vfs_node_t *vfs_finddir(vfs_node_t *dir,const char *name){
 for(int i=0;i<7;i++)if(parents[i]==dir-nodes&&!strcmp(nodes[i].name,name))return &nodes[i];
 return NULL;
}
static void vfs_close(vfs_node_t *n){(void)n;}
static uint32_t vfs_read(vfs_node_t *n,uint32_t off,uint32_t size,uint8_t *data){
 (void)n;(void)off;assert(size>=strlen(metadata));strcpy((char*)data,metadata);return strlen(metadata);
}
static int readlink_cb(vfs_node_t *n,char *dst,uint32_t size){
 (void)n;const char *s="../../../../class/sound";assert(size>strlen(s));strcpy(dst,s);return strlen(s);
}
static void *kmalloc(size_t n){return malloc(n);}
static void kfree(void *p){free(p);}
static uint64_t netlink_uevent_next_seqnum(void){return ++seq;}
static void netlink_broadcast(int protocol,uint32_t group,const void *data,size_t n){
 assert(protocol==15&&group==1&&n<sizeof(received));memcpy(received,data,n);received_len=n;
}
'''
checks = r'''
static bool field(const char *value){
 for(size_t i=0;i<received_len;i+=strlen(received+i)+1)if(!strcmp(received+i,value))return true;
 return false;
}
int main(void){
 const char *names[]={"sys","devices","sound","card0","controlC0","uevent","subsystem"};
 for(int i=0;i<7;i++){strcpy(nodes[i].name,names[i]);nodes[i].flags=i<5?FS_DIRECTORY:1;}
 nodes[6].readlink=readlink_cb;uevent_root=&nodes[0];
 sysfs_enable_uevent(&nodes[5],&nodes[4]);assert(nodes[5].mask==0644);
 char command[]="add 01234567-89ab-cdef-0123-456789abcdef\n";
 assert(nodes[5].write(&nodes[5],0,strlen(command),(uint8_t*)command)==strlen(command));
 assert(field("add@/devices/sound/card0/controlC0")&&field("ACTION=add"));
 assert(field("DEVPATH=/devices/sound/card0/controlC0")&&field("SUBSYSTEM=sound")&&field("SEQNUM=1"));
 assert(field("SYNTH_UUID=01234567-89ab-cdef-0123-456789abcdef")&&field("MAJOR=116"));
 assert(!strcmp(metadata,"MAJOR=116\nMINOR=0\nDEVNAME=snd/controlC0\nSUBSYSTEM=sound\n"));
 assert(nodes[5].write(&nodes[5],0,7,(uint8_t*)"change\n")==7);
 assert(field("ACTION=change")&&field("SEQNUM=2"));
 assert((int32_t)nodes[5].write(&nodes[5],1,3,(uint8_t*)"add")==-22);
 assert((int32_t)nodes[5].write(&nodes[5],0,4,(uint8_t*)"nope")==-22);
 assert((int32_t)nodes[5].write(&nodes[5],0,8,(uint8_t*)"add nope")==-22);
 assert(seq==2);
 puts("PASS: sysfs commands emit canonical events, carry UUID/sequence, preserve metadata, reject invalid writes");
}
'''
with tempfile.TemporaryDirectory(prefix='avory-uevent-') as temp:
    path = Path(temp)
    (path / 'test.c').write_text(fixture + source[start:end] + checks)
    subprocess.run(['cc', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
                    str(path / 'test.c'), '-o', str(path / 'test')], check=True)
    subprocess.run([str(path / 'test')], check=True)
