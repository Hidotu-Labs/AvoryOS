#!/usr/bin/env python3
"""Firefox-like .part write/rename/reopen/mmap on a private QEMU disk."""
import os
import subprocess
from boot_probe import run_probe

INIT = r'''
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/reboot.h>
#include <sys/stat.h>
#include <unistd.h>
#define LENGTH 117723467u
#define CHUNK 65521u
static unsigned char data[CHUNK];
static unsigned char expected(uint32_t offset) {
 uint32_t v=offset*2654435761u;v^=v>>13;v*=2246822519u;return (unsigned char)(v^(v>>16));
}
static void require(int ok,const char *what) {
 if(ok)return;printf("DOWNLOAD_INTEGRITY_BOOT: FAIL %s errno=%d\n",what,errno);
 fflush(stdout);reboot(RB_POWER_OFF);for(;;)pause();
}
int main(void) {
 setvbuf(stdout,NULL,_IOLBF,0);puts("DOWNLOAD_INTEGRITY_BOOT: START");
 int fd=open("/payload.part",O_CREAT|O_TRUNC|O_RDWR,0700);require(fd>=0,"create");
 /* Firefox may reserve capacity while preserving the visible .part size. */
 require(fallocate(fd,1,0,LENGTH)==0,"KEEP_SIZE reservation");
 struct stat st;require(!fstat(fd,&st)&&st.st_size==0,"reservation size");
 /* Reproduce Firefox's target-change continuation: close the first chunk,
  * move the partial file, then reopen in append mode with f_pos still zero. */
 for(uint32_t i=0;i<21870;i++)data[i]=expected(i);
 require(write(fd,data,21870)==21870,"first chunk");
 require(!close(fd),"close first chunk");
 require(!rename("/payload.part","/continued.part"),"move partial target");
 fd=open("/continued.part",O_WRONLY|O_APPEND);require(fd>=0,"reopen append");
 require(lseek(fd,0,SEEK_SET)==0,"append after explicit seek");
 for(uint32_t off=21870;off<LENGTH;){
  uint32_t n=LENGTH-off;if(n>CHUNK)n=CHUNK;
  for(uint32_t i=0;i<n;i++)data[i]=expected(off+i);
  ssize_t written=write(fd,data,n);require(written>0&&written<=n,"write");off+=written;
 }
 require(!fsync(fd),"fsync");require(!close(fd),"close");
 require(!rename("/continued.part","/payload.AppImage"),"rename");
 fd=open("/payload.AppImage",O_RDONLY);require(fd>=0,"reopen");
 require(!fstat(fd,&st)&&st.st_size==LENGTH,"completed size");
 for(uint32_t off=0;off<LENGTH;){
  uint32_t n=LENGTH-off;if(n>CHUNK)n=CHUNK;
  ssize_t got=read(fd,data,n);require(got>0&&got<=n,"read");
  for(uint32_t i=0;i<(uint32_t)got;i++)require(data[i]==expected(off+i),"read byte mismatch");
  off+=got;
 }
 puts("DOWNLOAD_INTEGRITY_BOOT: readback passed");
 unsigned char *map=mmap(NULL,LENGTH,PROT_READ,MAP_PRIVATE,fd,0);
 require(map!=MAP_FAILED,"private mmap");
 for(uint32_t i=0;i<LENGTH;i++)require(map[i]==expected(i),"mmap byte mismatch");
 require(!munmap(map,LENGTH),"munmap");close(fd);
 puts("DOWNLOAD_INTEGRITY_BOOT: PASS");fflush(stdout);
 reboot(RB_POWER_OFF);for(;;)pause();
}
'''
if __name__ == '__main__':
    filesystem=os.environ.get('AVORY_TEST_FILESYSTEM','ext4')
    def prepare(disk, out):
        # Host mke2fs defaults include ext4 features this kernel rejects.
        features = 'none,filetype,sparse_super'
        if filesystem == 'ext4':
            features += ',extent,has_journal'
        elif filesystem == 'ext3':
            features += ',has_journal'
        subprocess.run(['mke2fs', '-q', '-F', '-t', filesystem, '-O', features,
                        '-d', str(out/'root'), str(disk)], check=True)

    name = 'download-integrity-boot-' + filesystem
    prefix = name.upper().replace('-', '_')
    run_probe(name,INIT.replace('DOWNLOAD_INTEGRITY_BOOT', prefix),prefix + ': PASS',
              filesystem=filesystem,accel=os.environ.get('AVORY_TEST_ACCEL','tcg'),
              cpus=2,disk_mib=384,timeout=240,prepare_disk=prepare)
