#!/usr/bin/env python3
"""Real i386 TLS, fork/context switching and vectored I/O regression."""
import subprocess
from boot_probe import run_probe
CHILD=r'''
typedef unsigned int u32;
static long call(u32 n,u32 a,u32 b,u32 c,u32 d,u32 e){long r;__asm__ volatile("int $0x80":"=a"(r):"a"(n),"b"(a),"c"(b),"d"(c),"S"(d),"D"(e):"memory");return r;}
static long call6(u32 n,u32 a,u32 b,u32 c,u32 d,u32 e,u32 f){long r;register u32 sixth __asm__("ebp")=f;__asm__ volatile("int $0x80":"=a"(r):"a"(n),"b"(a),"c"(b),"d"(c),"S"(d),"D"(e),"r"(sixth):"memory");return r;}
static void finish(int code){call(1,code,0,0,0,0);for(;;);}
#define CHECK(x) do{if(!(x))finish(__LINE__);}while(0)
static volatile u32 first=0x12345678,second=0x89abcdef;
struct desc {u32 entry,base,limit,flags;};
static u32 gs_word(void){u32 x;__asm__ volatile("mov %%gs:0,%0":"=r"(x));return x;}
static struct desc desc={-1,0,0xfffff,0x51};
static u32 rseq_area[8] __attribute__((aligned(32)));
void _start(void){
 u32 robust[3]={(u32)robust,0,0};
 CHECK(call(311,(u32)robust,12,0,0,0)==0);
 CHECK(call(311,(u32)robust,24,0,0,0)==-22);
 CHECK(call(386,(u32)rseq_area,32,0,0x53053053,0)==0);
 CHECK(rseq_area[0]==rseq_area[1]);
 CHECK(call(386,(u32)rseq_area,32,1,0x53053053,0)==0);
 u32 limits[4]={0,0,0xa5a5a5a5,0xa5a5a5a5};
 CHECK(call(191,7,(u32)limits,0,0,0)==0);
 CHECK(limits[0]>0 && limits[2]==0xa5a5a5a5 && limits[3]==0xa5a5a5a5);
 unsigned char metadata[112];for(unsigned i=0;i<112;i++)metadata[i]=0xa5;
 const char path[]="/bin/compat-tls";
 CHECK(call(195,(u32)path,(u32)metadata,0,0,0)==0);
 for(unsigned i=96;i<112;i++)CHECK(metadata[i]==0xa5);
 long file=call(5,(u32)path,0,0,0,0);CHECK(file>=0);
 CHECK(call(197,file,(u32)metadata,0,0,0)==0);call(6,file,0,0,0,0);

 CHECK(call(243,1,0,0,0,0)==-14);
 desc.entry=5;CHECK(call(243,(u32)&desc,0,0,0,0)==-22);
 desc.entry=-1;desc.base=(u32)&first;
 CHECK(call(243,(u32)&desc,0,0,0,0)==0 && desc.entry==6);
 unsigned short selector=(desc.entry<<3)|3;
 __asm__ volatile("mov %0,%%gs"::"rm"(selector):"memory");
 CHECK(gs_word()==first);
 u32 futex_word=7,other_word=0;int timeout[2]={0,0};
 CHECK(call(240,(u32)&futex_word,128,8,0,0)==-11);
 CHECK(call(240,(u32)&futex_word,128,7,(u32)timeout,0)==-110);
 CHECK(call(240,(u32)&futex_word,128,7,1,0)==-14);
 timeout[1]=1000000000;
 CHECK(call(240,(u32)&futex_word,128,7,(u32)timeout,0)==-22);

 long me=call(20,0,0,0,0,0),tid=call(224,0,0,0,0,0);
 CHECK(call(270,me,tid,0,0,0)==0);
 CHECK(call(270,-1,tid,0,0,0)==-22);
 struct __attribute__((packed)) {u32 handler,flags,restorer;unsigned long long mask;} action={1,0,0,0};
 unsigned char previous[28];for(unsigned i=0;i<28;i++)previous[i]=0xa5;
 CHECK(call(174,13,(u32)&action,(u32)previous,8,0)==0);
 CHECK(call(174,13,0,(u32)previous,8,0)==0 && previous[0]==1);
 for(unsigned i=20;i<28;i++)CHECK(previous[i]==0xa5);
 action.handler=0x12345678;CHECK(call(174,13,(u32)&action,0,8,0)==-95);
 action.handler=0;CHECK(call(174,9,(u32)&action,0,8,0)==-22);


 struct desc child_tls={6,(u32)&second,0xfffff,0x51};
 long cloned=call6(120,0x80000|17,0,0,(u32)&child_tls,0,0);CHECK(cloned>=0);
 if(!cloned){CHECK(gs_word()==second);finish(0);}
 int clone_status=-1;CHECK(call(7,cloned,(u32)&clone_status,0,0,0)==cloned && clone_status==0);
 CHECK(gs_word()==first);
 long seek_fd=call(5,(u32)"/bin/compat-tls",0,0,0,0);CHECK(seek_fd>=0);
 unsigned long long position=0;
 CHECK(call6(140,seek_fd,1,123,(u32)&position,0,0)==0 && position==0x10000007bULL);
 CHECK(call6(140,seek_fd,0,0,(u32)&position,1,0)==0 && position==0x10000007bULL);
 call(6,seek_fd,0,0,0,0);
 long sock=call(359,1,1,0,0,0);CHECK(sock>=0);call(6,sock,0,0,0,0);
 int pair[2];CHECK(call(360,1,1,0,(u32)pair,0)==0);
 CHECK(call(369,pair[0],(u32)"sdl",3,0,0)==3);
 char received[3];CHECK(call6(371,pair[1],(u32)received,3,0,0,0)==3);
 CHECK(received[0]=='s' && received[1]=='d' && received[2]=='l');
 call(6,pair[0],0,0,0,0);call(6,pair[1],0,0,0,0);
 /* Updating the already-loaded descriptor must refresh its hidden base. */
 desc.base=(u32)&second;CHECK(call(243,(u32)&desc,0,0,0,0)==0);
 CHECK(gs_word()==second);
 desc.base=(u32)&first;CHECK(call(243,(u32)&desc,0,0,0,0)==0);
 long mapped=call6(192,0,4096,3,0x21,-1,0);
 CHECK((u32)mapped < (u32)-4095);
 volatile u32 *shared=(volatile u32 *)(u32)mapped;
 shared[0]=0;shared[1]=0;
 long pid=call(2,0,0,0,0,0);CHECK(pid>=0);
 if(!pid){
   u32 owned_head[3]={(u32)shared,4,0};
   shared[0]=(u32)owned_head;shared[1]=call(224,0,0,0,0,0);
   CHECK(call(311,(u32)owned_head,12,0,0,0)==0);
   CHECK(gs_word()==first);
   desc.base=(u32)&second;CHECK(call(243,(u32)&desc,0,0,0,0)==0);
   for(unsigned i=0;i<1000;i++){CHECK(gs_word()==second);call(158,0,0,0,0,0);}
   finish(0);
 }
 for(unsigned i=0;i<1000;i++){CHECK(gs_word()==first);call(158,0,0,0,0,0);}
 int status=-1;CHECK(call(7,pid,(u32)&status,0,0,0)==pid && status==0);
 CHECK(shared[1]==0x40000000);
 CHECK(call(91,(u32)shared,4096,0,0,0)==0);
 struct desc extra={-1,(u32)&second,0xfffff,0x51};
 CHECK(call(243,(u32)&extra,0,0,0,0)==0 && extra.entry==7);
 extra.entry=-1;CHECK(call(243,(u32)&extra,0,0,0,0)==0 && extra.entry==8);
 extra.entry=-1;CHECK(call(243,(u32)&extra,0,0,0,0)==-3);
 extra=(struct desc){7,0,0,0x28};CHECK(call(243,(u32)&extra,0,0,0,0)==0);
 extra=(struct desc){-1,(u32)&second,0xfffff,0x51};
 CHECK(call(243,(u32)&extra,0,0,0,0)==0 && extra.entry==7);
 int fds[2];CHECK(call(42,(u32)fds,0,0,0,0)==0);
 char a[]="abc",b[]="def";struct {u32 base,length;} vec[2]={{(u32)a,3},{(u32)b,3}};
 CHECK(call(146,fds[1],(u32)vec,2,0,0)==6);
 char x[3]={0},y[3]={0};vec[0].base=(u32)x;vec[1].base=(u32)y;
 CHECK(call(145,fds[0],(u32)vec,2,0,0)==6);
 for(unsigned i=0;i<3;i++){CHECK(x[i]==a[i]);CHECK(y[i]==b[i]);}
 CHECK(call(146,fds[1],1,1,0,0)==-14);
 CHECK(call(146,fds[1],1,0,0,0)==0);
 CHECK(call(146,fds[1],(u32)vec,1025,0,0)==-22);
 call(6,fds[0],0,0,0,0);call(6,fds[1],0,0,0,0);
 finish(0);
}
'''
INIT=r'''
#include <stdio.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/reboot.h>
int main(void){setbuf(stdout,NULL);puts("COMPAT_RUNTIME_SYNC_BOOT: START");
 int status=0;pid_t p=fork();if(!p){execl("/bin/compat-tls","compat-tls",NULL);_exit(127);}
 if(p<0||waitpid(p,&status,0)!=p)status=0xffff;
 printf("COMPAT_RUNTIME_SYNC_BOOT: %s status=%d\n",status?"FAIL":"PASS",status);
 reboot(RB_POWER_OFF);for(;;)pause();}
'''
def setup(root):
 source=root.parent/'compat-tls.c';source.write_text(CHILD)
 subprocess.run(['cc','-m32','-O1','-fomit-frame-pointer','-fno-builtin','-fno-pie','-fno-stack-protector','-nostdlib','-static','-Wl,-e,_start',str(source),'-o',str(root/'bin/compat-tls')],check=True)
if __name__=='__main__':
 run_probe('compat-runtime-sync-boot',INIT,'COMPAT_RUNTIME_SYNC_BOOT: PASS',setup=setup,timeout=90)
