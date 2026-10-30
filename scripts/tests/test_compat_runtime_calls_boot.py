#!/usr/bin/env python3
"""Check narrow statfs, pselect, prctl and network ioctl outputs in a guest."""
import test_compat_stat_boot as base
from boot_probe import run_probe
extra=r'''
 for(unsigned i=0;i<sizeof(st);i++)st[i]=0xa5;
 CHECK(call(268,(u32)"/",84,(u32)st,0,0)==0);
 for(unsigned i=84;i<sizeof(st);i++)CHECK(st[i]==0xa5);
 CHECK(call(268,(u32)"/",83,(u32)st,0,0)==-22);
 CHECK(call(268,(u32)"/",84,1,0,0)==-14);
 CHECK(call(97,0,0,0,0,0)==0);
 u32 tid[2]={0,0x12345678};
 CHECK(call(172,40,(u32)tid,0,0,0)==0);
 CHECK(tid[1]==0x12345678);
 int pipefd[2]; CHECK(call(42,(u32)pipefd,0,0,0,0)==0);
 CHECK(call(4,pipefd[1],(u32)"x",1,0,0)==1);
 u32 set[2]={1u<<pipefd[0],0xa5a5a5a5}; int timeout[2]={0,0};
 CHECK(call(308,pipefd[0]+1,(u32)set,0,0,(u32)timeout)==1);
 CHECK(set[0]==(1u<<pipefd[0]) && set[1]==0xa5a5a5a5);
 CHECK(call(221,pipefd[0],1032,0,0,0)>0);
 unsigned char lock[28]={0}; lock[0]=1;
 CHECK(call(221,pipefd[0],12,(u32)lock,0,0)==0);
 CHECK(lock[0]==2 && lock[1]==0);
 CHECK(call(221,pipefd[0],12,1,0,0)==-14);
 CHECK(call(221,pipefd[0],0x7fffffff,0,0,0)==-22);
 int fd=call(359,2,2,0,0,0); CHECK(fd>=0);
 u32 conf[3]={0,0,0x12345678};
 CHECK(call(54,fd,0x8912,(u32)conf,0,0)==0);
 CHECK(conf[2]==0x12345678 && conf[0]%32==0);
 unsigned char req[40]={ 'l','o',0 };
 for(unsigned i=32;i<40;i++)req[i]=0xa5;
 CHECK(call(54,fd,0x8913,(u32)req,0,0)==0);
 for(unsigned i=32;i<40;i++)CHECK(req[i]==0xa5);
 call(6,fd,0,0,0,0);
'''
base.CHILD=base.CHILD.replace('int $0x80', 'push %%ebp; xor %%ebp,%%ebp; int $0x80; pop %%ebp').replace(' finish(0);',extra+'\n finish(0);')
if __name__=='__main__':
 run_probe('compat-runtime-calls',base.INIT.replace('COMPAT_STAT_BOOT','COMPAT_RUNTIME_CALLS'),
           'COMPAT_RUNTIME_CALLS: PASS',setup=base.setup,timeout=90)
