#!/usr/bin/env python3
"""Out-of-order torrent-style writes across ext4 index splits in a private VM."""
import subprocess
from boot_probe import run_probe
INIT = r'''
#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <sys/reboot.h>
#define COUNT 1600
static unsigned char data[1024],actual[1024];
static void check(int ok,const char *what,unsigned key){
 if(ok)return;
 printf("EXT4_RANDOM_WRITE_BOOT: FAIL %s key=%u errno=%d\n",what,key,errno);
 reboot(RB_POWER_OFF);for(;;)pause();
}
static void pattern(unsigned key){for(unsigned i=0;i<sizeof(data);i++)data[i]=(unsigned char)(key*17+i*13+(key>>8));}
int main(void){
 setbuf(stdout,NULL);puts("EXT4_RANDOM_WRITE_BOOT: START");
 int fd=open("/torrent.part",O_CREAT|O_TRUNC|O_RDWR,0600);check(fd>=0,"open",0);
 for(unsigned i=0;i<COUNT;i++){
  unsigned key=(i*997+1200)%COUNT;pattern(key);
  check(pwrite(fd,data,sizeof(data),(off_t)key*3072)==sizeof(data),"pwrite",key);
 }
 check(!fsync(fd),"fsync",0);close(fd);
 fd=open("/torrent.part",O_RDONLY);check(fd>=0,"reopen",0);
 for(unsigned key=0;key<COUNT;key++){
  pattern(key);check(pread(fd,actual,sizeof(actual),(off_t)key*3072)==sizeof(actual),"pread",key);
  check(!memcmp(data,actual,sizeof(data)),"bytes",key);
 }
 close(fd);sync();puts("EXT4_RANDOM_WRITE_BOOT: PASS");
 reboot(RB_POWER_OFF);for(;;)pause();
}
'''
def prepare(disk,out):
 subprocess.run(['mke2fs','-q','-F','-t','ext4','-b','1024','-I','128',
                 '-O','none,filetype,sparse_super,extent,has_journal',
                 '-d',str(out/'root'),str(disk)],check=True)
if __name__=='__main__':
 run_probe('ext4-random-write-boot',INIT,'EXT4_RANDOM_WRITE_BOOT: PASS',
           filesystem='ext4',prepare_disk=prepare,accel='tcg',cpus=2,
           disk_mib=64,timeout=180)
