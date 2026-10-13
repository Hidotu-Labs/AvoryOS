#!/usr/bin/env python3
"""Test actual restart policy and actual syscall-frame conversion.

--before uses captured old signal_deliver_syscall as a negative control.
"""
from pathlib import Path
import argparse,re,subprocess,tempfile
ROOT=Path(__file__).resolve().parents[2]
parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('--before',type=Path)
args=parser.parse_args()
signal=(args.before or ROOT/'kernel/src/syscalls/sys_signal.c').read_text()
policy=re.sub(r'^#include[^\n]*\n','',(ROOT/'kernel/src/syscalls/sys_restart.c').read_text(),flags=re.M)
if args.before:
    start=signal.index('void signal_deliver_syscall(')
    conversion=signal[start:signal.index('// tgkill, sigaltstack',start)]
    entry='signal_deliver_syscall(&s);'
else:
    start=signal.index('static void signal_syscall_return(')
    conversion=signal[start:signal.index('// tgkill, sigaltstack',start)]
    entry='signal_finish_syscall(&s,16,0x1002);'
pre=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#define SA_RESTART 0x10000000
struct registers {uint64_t rdi,rsi,rdx,r10,r8,r9,rax,rbx,rbp,r12,r13,r14,r15,rip,rflags,rcx,r11,rsp,cs,ss;};
struct syscall_regs {uint64_t rdi,rsi,rdx,r10,r8,r9,rax,rbx,rbp,r12,r13,r14,r15,rip,rflags,rsp,user_cs,user_rcx,user_rsi,user_rdi;};
'''
mock=r'''
static bool has_handler;
static uint64_t flags,frame_rax,frame_rip;
static void signal_deliver_frame(struct registers *r,uint64_t number,uint64_t ip){
 syscall_restart_return(r,number,ip,has_handler,flags);frame_rax=r->rax;frame_rip=r->rip;
}
static void signal_deliver(struct registers *r){frame_rax=r->rax;frame_rip=r->rip;}
'''
post=r'''
int main(void){
 struct syscall_regs s={.rax=(uint64_t)-512,.rip=0x1002,.rdi=7,.rsi=0xc00c642e,.rdx=0x7000,.rsp=0x8000,.rflags=0x202};
 has_handler=true;flags=SA_RESTART;
 @ENTRY@
 assert(s.rax==16 && s.rip==0x1000 && frame_rax==16 && frame_rip==0x1000);
 assert(s.rdi==7 && s.rsi==0xc00c642e && s.rdx==0x7000 && s.rsp==0x8000 && s.rflags==0x202);
 s.rax=(uint64_t)-512;s.rip=0x1002;flags=0;
 @ENTRY@
 assert((int64_t)s.rax==-4 && s.rip==0x1002 && (int64_t)frame_rax==-4);
 struct registers r={.rip=0x1002};
 for(int code=512;code<=516;code++){
  if(code==515)continue;
  for(int caught=0;caught<2;caught++)for(int restart=0;restart<2;restart++){
   r.rax=(uint64_t)-code;r.rip=0x1002;
   syscall_restart_return(&r,16,0x1002,caught,restart?SA_RESTART:0);
   bool expected=code==513 || (code==512&&(!caught||restart)) || (code==514&&!caught);
   assert(expected ? (r.rax==16&&r.rip==0x1000) : ((int64_t)r.rax==-4&&r.rip==0x1002));
  }
 }
 r.rax=(uint64_t)-512;r.rip=0x2000;
 syscall_restart_return(&r,16,0x1002,false,0);assert((int64_t)r.rax==-4 && r.rip==0x2000);
 r.rax=55;r.rip=0x1002;syscall_restart_return(&r,16,0x1002,false,0);assert(r.rax==55&&r.rip==0x1002);
 r.rax=(uint64_t)-512;syscall_restart_return(&r,UINT64_MAX,0,false,0);assert((int64_t)r.rax==-512);
 puts("PASS: real syscall-frame conversion, SA_RESTART/no-handler policies, EINTR, argument preservation and redirected-IP safety");
}
'''
with tempfile.TemporaryDirectory(prefix='avory-restart-') as tmp:
    p=Path(tmp);(p/'check.c').write_text(pre+policy+mock+conversion+post.replace('@ENTRY@',entry))
    subprocess.run(['cc','-O1','-g','-Wall','-Wextra','-Werror','-Wno-unused-function',
        '-fsanitize=address,undefined',p/'check.c','-o',p/'check'],check=True)
    subprocess.run([p/'check'],check=True,timeout=10)
