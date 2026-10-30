#!/usr/bin/env python3
"""ELF execution from tmpfs must zero partial-page and beyond-EOF BSS."""
import struct
from boot_probe import run_probe

INIT = r'''
#define _GNU_SOURCE
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/reboot.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
static unsigned char bss[32768];
static volatile uint32_t initialized=0x12345678;
static void child_exit(int sig){(void)sig;}
static int check_bss(void) {
 if(initialized!=0x12345678)return 2;
 /* Split the segment inside its pure BSS range. A zero initialized-byte
  * count must survive mprotect without becoming a file-backed page. */
 uintptr_t page=((uintptr_t)bss+4095)&~(uintptr_t)4095;
 if(mprotect((void *)(page+8192),4096,PROT_READ))return 4;
 for(unsigned i=0;i<sizeof(bss);i++)if(bss[i])return 3;
 volatile unsigned char *fresh=(void *)(page+8192);
 for(unsigned i=0;i<4096;i++)if(fresh[i])return 5;
 bss[0]=17;bss[sizeof(bss)-1]=29;
 return bss[0]==17 && bss[sizeof(bss)-1]==29 ? 0 : 6;
}
int main(int argc,char **argv) {
 if(argc>1 && !strcmp(argv[1],"--child"))return check_bss();
 setbuf(stdout,NULL);puts("TMPFS_ELF_BSS_BOOT: START");signal(SIGCHLD,child_exit);
 int in=open("/root/bss-child",O_RDONLY),out=open("/tmp/bss-child",O_CREAT|O_TRUNC|O_WRONLY,0755);
 int ok=in>=0 && out>=0;
 char buffer[8192];ssize_t n;
 while(ok && (n=read(in,buffer,sizeof(buffer)))>0)if(write(out,buffer,n)!=n)ok=0;
 close(in);close(out);
 pid_t child=ok ? fork() : -1;
 if(!child){execl("/tmp/bss-child","bss-child","--child",NULL);_exit(127);}
 int status=-1;
 if(child<0 || waitpid(child,&status,0)!=child || status)ok=0;
 /* Loading and zeroing BSS must leave the underlying executable untouched. */
 in=open("/root/bss-child",O_RDONLY);out=open("/tmp/bss-child",O_RDONLY);
 char other[8192];
 while(ok && (n=read(in,buffer,sizeof(buffer)))>0)
  if(read(out,other,n)!=n || memcmp(buffer,other,n))ok=0;
 close(in);close(out);
 printf("TMPFS_ELF_BSS_BOOT: %s status=%d\n",ok?"PASS":"FAIL",status);
 reboot(RB_POWER_OFF);for(;;)pause();
}
'''


def setup(root):
    image = bytearray((root / 'bin/probe-init').read_bytes())
    phoff = struct.unpack_from('<Q', image, 32)[0]
    phsize, phnum = struct.unpack_from('<HH', image, 54)
    poisoned = False
    for i in range(phnum):
        kind, flags, offset, va, pa, filesz, memsz, align = struct.unpack_from(
            '<IIQQQQQQ', image, phoff + i * phsize)
        if kind == 1 and memsz > filesz and filesz and (offset + filesz) % 4096:
            end = offset + filesz
            tail = (end + 4095) & ~4095
            if len(image) < tail:
                image.extend(b'\0' * (tail - len(image)))
            image[end:tail] = b'\xa5' * (tail - end)
            poisoned = True
    assert poisoned, 'fixture needs a partial initialized segment with BSS'
    target = root / 'root/bss-child'
    target.write_bytes(image)
    target.chmod(0o755)


if __name__ == '__main__':
    run_probe('tmpfs-elf-bss-boot', INIT, 'TMPFS_ELF_BSS_BOOT: PASS',
              setup=setup, accel='tcg', cpus=1, timeout=90)
