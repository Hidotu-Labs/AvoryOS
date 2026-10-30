#!/usr/bin/env python3
"""Exercise real i386 statx/fstatat64 syscalls in an AvoryOS guest."""
import subprocess
from boot_probe import run_probe
CHILD=r'''
typedef unsigned int u32;
typedef unsigned long long u64;
static long call(u32 n,u32 a,u32 b,u32 c,u32 d,u32 e){long r;__asm__ volatile("int $0x80":"=a"(r):"a"(n),"b"(a),"c"(b),"d"(c),"S"(d),"D"(e):"memory");return r;}
static void finish(int code){call(1,code,0,0,0,0);for(;;);}
#define CHECK(x) do{if(!(x))finish(__LINE__);}while(0)
static unsigned char sx[256] __attribute__((aligned(8)));
static unsigned char st[112] __attribute__((aligned(8)));
static const char path[]="/bin/compat-stat", missing[]="/missing-stat-file", link[]="/stat-link";
static u32 word(unsigned char *p){u32 r;__builtin_memcpy(&r,p,4);return r;}
void _start(void){
 for(unsigned i=0;i<sizeof(st);i++)st[i]=0xa5;
 CHECK(call(383,-100,(u32)path,0,0x7ff,(u32)sx)==0);
 CHECK(call(300,-100,(u32)path,(u32)st,0,0)==0);
 CHECK((word(st+16)&0170000)==0100000);
 CHECK((word(sx+28)&65535)==word(st+16));
 CHECK(word(st+44)==word(sx+40) && word(st+48)==word(sx+44));
 CHECK(word(st+88)==word(sx+32) && word(st+92)==word(sx+36));
 for(unsigned i=96;i<sizeof(st);i++)CHECK(st[i]==0xa5);
 CHECK(call(300,-100,(u32)missing,(u32)st,0,0)==-2);
 CHECK(call(300,-100,(u32)path,1,0,0)==-14);
 CHECK(call(300,-100,(u32)path,(u32)st,0x40000000,0)==-22);
 CHECK(call(300,-100,(u32)link,(u32)st,0x100,0)==0);
 CHECK((word(st+16)&0170000)==0120000);
 CHECK(call(300,-100,(u32)link,(u32)st,0,0)==0);
 CHECK((word(st+16)&0170000)==0100000);
 finish(0);
}
'''
INIT=r'''
#include <stdio.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/reboot.h>
int main(void){setbuf(stdout,NULL);puts("COMPAT_STAT_BOOT: START");
 int status=0;pid_t p=fork();if(!p){execl("/bin/compat-stat","compat-stat",NULL);_exit(127);}
 if(p<0||waitpid(p,&status,0)!=p)status=0xffff;
 printf("COMPAT_STAT_BOOT: %s status=%d\n",status?"FAIL":"PASS",status);
 reboot(RB_POWER_OFF);for(;;)pause();}
'''
def setup(root):
 source=root.parent/'compat-stat.c';source.write_text(CHILD)
 subprocess.run(['cc','-m32','-O1','-fno-builtin','-fno-pie','-fno-stack-protector','-nostdlib','-static','-Wl,-e,_start',str(source),'-o',str(root/'bin/compat-stat')],check=True)
 (root/'stat-link').symlink_to('/bin/compat-stat')
if __name__=='__main__':
 run_probe('compat-stat-boot',INIT,'COMPAT_STAT_BOOT: PASS',setup=setup,timeout=90)
