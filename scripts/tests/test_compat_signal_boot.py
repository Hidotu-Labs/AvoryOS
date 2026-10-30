#!/usr/bin/env python3
"""Exercise i386 handlers, both sigreturn ABIs, TLS, SSE and fault contexts."""
import subprocess
from boot_probe import run_probe
from test_compat_stat_boot import INIT

CHILD = r'''
typedef unsigned int u32;
static long call(u32 n,u32 a,u32 b,u32 c,u32 d,u32 e){long r;
 __asm__ volatile("int $0x80":"=a"(r):"a"(n),"b"(a),"c"(b),"d"(c),"S"(d),"D"(e):"memory");return r;}
static void finish(int code){call(1,code,0,0,0,0);for(;;);}
#define CHECK(x) do{if(!(x))finish(__LINE__);}while(0)
struct action {u32 handler,flags,restorer,mask[2];};
struct sc {u32 gs,fs,es,ds,di,si,bp,sp,bx,dx,cx,ax,trap,err,ip,cs,flags,sp_at_signal,ss,fp,oldmask,cr2;};
struct uc {u32 flags,link,stack[3];struct sc sc;u32 mask[2];};
static volatile u32 handled,real_time,stage,nested;
static u32 pid,tls=0x1234abcd;
static unsigned char alt[16384] __attribute__((aligned(64)));
static const u32 pattern[4]={0x11112222,0x33334444,0x55556666,0x77778888};
static u32 actual[4];
__attribute__((naked)) static void old_return(void){__asm__ volatile("pop %eax; mov $119,%eax; int $0x80; ud2");}
__attribute__((naked)) static void rt_return(void){__asm__ volatile("mov $173,%eax; int $0x80; ud2");}
static void legacy(int sig){
 CHECK(sig==10);handled++;
 u32 mask[2]={0}; CHECK(call(175,0,0,(u32)mask,8,0)==0);
 CHECK(mask[0]&(1u<<9));CHECK(mask[1]&(1u<<12));
 __asm__ volatile("pxor %%xmm0,%%xmm0":::"xmm0");
}
static void modern(int sig,u32 *info,struct uc *uc){
 CHECK(info[0]==(u32)sig);CHECK(uc->sc.cs==0x1b && uc->sc.ss==0x23);
 u32 value;__asm__ volatile("mov %%gs:0,%0":"=r"(value));CHECK(value==tls);
 if(stage==1){CHECK(sig==10 && info[3]==pid);uc->sc.ax=0x1234;}
 if(stage==2){u32 local;CHECK((u32)&local>=(u32)alt && (u32)&local<(u32)alt+sizeof(alt));}
 if(stage==3){CHECK(sig==4);uc->sc.ip+=2;}
 if(stage==4 && !nested++){CHECK(call(37,pid,10,0,0,0)==0);}
 if(stage==5)uc->sc.cs=0;
 if(stage==6)uc->sc.fp=1;
 if(stage==7)((u32*)uc->sc.fp)[34]=0xffffffff; /* FXSR MXCSR */
 real_time++;
}
void _start(void){
 pid=call(20,0,0,0,0,0);
 u32 desc[4]={6,(u32)&tls,0xfffff,0x51};
 CHECK(call(243,(u32)desc,0,0,0,0)==0);
 __asm__ volatile("mov $0x33,%%ax; mov %%ax,%%gs":::"eax");
 u32 blocked[2]={0,1u<<12};CHECK(call(175,2,(u32)blocked,0,8,0)==0);
 struct action a={(u32)legacy,0,0,{0,0}};
 CHECK(call(174,10,(u32)&a,0,8,0)==0);
 __asm__ volatile("movdqu %0,%%xmm0"::"m"(pattern):"xmm0");
 CHECK(call(37,pid,10,0,0,0)==0);
 __asm__ volatile("movdqu %%xmm0,%0":"=m"(actual));
 for(unsigned i=0;i<4;i++)CHECK(actual[i]==pattern[i]);
 CHECK(handled==1);
 a.flags=0x04000000;a.restorer=(u32)old_return;
 CHECK(call(174,10,(u32)&a,0,8,0)==0);CHECK(call(37,pid,10,0,0,0)==0);CHECK(handled==2);
 a.handler=(u32)modern;a.flags=4;a.restorer=0;stage=1;
 CHECK(call(174,10,(u32)&a,0,8,0)==0);CHECK(call(37,pid,10,0,0,0)==0x1234);
 a.flags=4|0x04000000;a.restorer=(u32)rt_return;
 CHECK(call(174,10,(u32)&a,0,8,0)==0);CHECK(call(37,pid,10,0,0,0)==0x1234);
 stage=2;u32 stack[3]={(u32)alt,0,sizeof(alt)},oldstack[4]={0,0,0,0x12345678};
 CHECK(call(186,(u32)stack,(u32)oldstack,0,0,0)==0);CHECK(oldstack[3]==0x12345678);
 a.flags|=0x08000000;
 CHECK(call(174,10,(u32)&a,0,8,0)==0);CHECK(call(37,pid,10,0,0,0)==0);
 stage=3;a.flags=4|0x04000000;
 CHECK(call(174,4,(u32)&a,0,8,0)==0);__asm__ volatile("ud2");
 stage=4;a.flags|=0x40000000;
 CHECK(call(174,10,(u32)&a,0,8,0)==0);CHECK(call(37,pid,10,0,0,0)==0);CHECK(nested==2);
 u32 mask[2]={0};CHECK(call(175,0,0,(u32)mask,8,0)==0);CHECK(mask[0]==0 && mask[1]==blocked[1]);
 CHECK(real_time==6);
 for(stage=5;stage<=7;stage++){
  long child=call(2,0,0,0,0,0);CHECK(child>=0);
  if(!child){pid=call(20,0,0,0,0,0);call(37,pid,10,0,0,0);finish(119);}
  int status=0;CHECK(call(7,child,(u32)&status,0,0,0)==child);CHECK(status!=0);
 }
 finish(0);
}
'''

def setup(root):
    source = root.parent / 'compat-signal.c'
    source.write_text(CHILD)
    subprocess.run(['cc', '-m32', '-msse2', '-O1', '-fno-builtin', '-fno-pie',
                    '-fno-stack-protector', '-nostdlib', '-static', '-Wl,-e,_start',
                    str(source), '-o', str(root / 'bin/compat-stat')], check=True)

if __name__ == '__main__':
    run_probe('compat-signal-boot', INIT.replace('COMPAT_STAT_BOOT', 'COMPAT_SIGNAL_BOOT'),
              'COMPAT_SIGNAL_BOOT: PASS', setup=setup, cpus=2, accel='tcg', timeout=120)
