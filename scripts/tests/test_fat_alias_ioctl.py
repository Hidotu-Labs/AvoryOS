#!/usr/bin/env python3
"""Exercise the actual FAT alias reader with synthetic directory entries."""
from pathlib import Path
import subprocess
import tempfile
ROOT=Path(__file__).resolve().parents[2]
s=(ROOT/'kernel/src/fs/fat32.c').read_text();start=s.index('int fat32_read_aliases(')
code=s[start:s.index('// Find a file in a directory',start)]
h=(ROOT/'kernel/src/fs/fat32.h').read_text();start=h.index('struct fat32_ioctl_dirent {');abi=h[start:h.index('int fat32_read_aliases(',start)]
prefix=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define FAT32_ATTR_VOLUME_ID 8
#define FAT32_LFN_LAST_ENTRY 0x40
typedef struct { unsigned char name[11],attr; uint16_t cluster_low,cluster_high; } fat32_dir_entry_t;
typedef struct { uint8_t seq; unsigned char padding[10],attr; } fat32_lfn_entry_t;
typedef struct { unsigned root_cluster; } fat32_mount_t;
typedef struct { void *device; unsigned impl; void *readdir; } vfs_node_t;
typedef struct { fat32_lfn_entry_t lfn_entries[20]; int lfn_count; unsigned cursor; } dir_iter_t;
static fat32_dir_entry_t entries[8];static unsigned entry_count, cleanup;static bool fail_init;static int decode_length=17;
static void fat32_readdir_impl(void) {}
static int dir_iter_init(dir_iter_t *i,fat32_mount_t *m,unsigned cluster) {
 (void)m;assert(cluster==2);memset(i,0,sizeof(*i));return fail_init?-1:0;
}
static fat32_dir_entry_t *dir_iter_next(dir_iter_t *i) {return i->cursor<entry_count?&entries[i->cursor++]:NULL;}
static void dir_iter_free(dir_iter_t *i) {(void)i;cleanup++;}
static bool is_lfn_entry(fat32_dir_entry_t *e) { return e->attr==15; }
static int decode_lfn(fat32_lfn_entry_t *entries,int count,char *out,unsigned char *shortname) {
 (void)entries;(void)shortname;assert(count==1);if(decode_length==17)strcpy(out,"Long Filename.dll");else if(decode_length>0){memset(out,'x',decode_length);out[decode_length]=0;}return decode_length;
}
'''
tests=r'''
int main(void) {
 fat32_mount_t m={.root_cluster=2};vfs_node_t node={.device=&m,.readdir=fat32_readdir_impl};
 struct fat32_ioctl_dirent result[2];
 assert(fat32_read_aliases(NULL,0,false,result)==-25);
 vfs_node_t ext={0};assert(fat32_read_aliases(&ext,0,false,result)==-25);
 entries[0].name[0]=0x41;entries[0].attr=15;
 memcpy(entries[1].name,"LONGFI~1DLL",11);entries[1].cluster_low=42;
 memcpy(entries[2].name,"PLAIN   TXT",11);entry_count=3;
 assert(fat32_read_aliases(&node,0,false,result)==1);
 assert(!strcmp(result[0].name,"LONGFI~1.DLL") && result[0].reclen==12);
 assert(!strcmp(result[1].name,"Long Filename.dll") && result[1].ino==42 && result[1].offset==1);
 assert(fat32_read_aliases(&node,0,true,result)==1 && !result[1].name[0] && !result[1].reclen);
 assert(fat32_read_aliases(&node,1,false,result)==1 && !strcmp(result[0].name,"PLAIN.TXT") && !result[1].name[0]);
 assert(fat32_read_aliases(&node,2,false,result)==0 && !result[0].reclen);
 assert(cleanup==4);
 decode_length=260;assert(fat32_read_aliases(&node,0,false,result)==1 && !result[1].reclen);
 decode_length=-1;assert(fat32_read_aliases(&node,0,false,result)==1 && !result[1].reclen);
 fail_init=true;assert(fat32_read_aliases(&node,0,false,result)==-5);
 puts("PASS: native VFAT ABI, short/long pairs, short-only, cursor/EOF, filesystem rejection and I/O failure");
}
'''
with tempfile.TemporaryDirectory() as tmp:
 p=Path(tmp);(p/'test.c').write_text(prefix+abi+code+tests)
 subprocess.run(['cc','-std=gnu11','-Wall','-Wextra','-Werror','-fsanitize=undefined',str(p/'test.c'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
