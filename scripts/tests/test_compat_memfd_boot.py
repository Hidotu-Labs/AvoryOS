#!/usr/bin/env python3
"""Boot real i386 sparse memfd, mapping/COW and clock_nanosleep checks."""
import test_compat_stat_boot as base
from boot_probe import run_probe
base.CHILD=base.CHILD.replace('int $0x80','push %%ebp; xor %%ebp,%%ebp; int $0x80; pop %%ebp')
extra=r'''
 call(4,1,(u32)"MEMFD: create\n",14,0,0);
 int fd=call(356,(u32)"graphics",0,0,0,0); CHECK(fd>=0);
 CHECK(call(194,fd,512u*1024*1024,0,0,0)==0);
 CHECK(call(197,fd,(u32)st,0,0,0)==0 && word(st+44)==512u*1024*1024);
 call(4,1,(u32)"MEMFD: map\n",11,0,0);
 long shared=call(192,0,512u*1024*1024,3,1,fd); CHECK(shared>0);
 long priv=call(192,0,4096,3,2,fd); CHECK(priv>0);
 volatile u32 *s=(void*)shared,*p=(void*)priv;
 CHECK(*s==0 && *p==0); *s=0x12345678; CHECK(*p==0x12345678);
 *p=0x87654321; CHECK(*s==0x12345678 && *p==0x87654321);
 CHECK(call(194,fd,768u*1024*1024,0,0,0)==0);
 *s=0x13572468; CHECK(call(19,fd,0,0,0,0)==0);
 u32 value=0; CHECK(call(3,fd,(u32)&value,4,0,0)==4 && value==*s);
 CHECK(call(6,fd,0,0,0,0)==0);
 CHECK(*s==0x13572468 && *p==0x87654321);
 CHECK(call(91,shared,512u*1024*1024,0,0,0)==0);
 CHECK(call(91,priv,4096,0,0,0)==0);
 call(4,1,(u32)"MEMFD: clock\n",13,0,0);
 unsigned char si[80];for(unsigned i=0;i<80;i++)si[i]=0xa5;
 CHECK(call(116,(u32)si,0,0,0,0)==0);
 CHECK(word(si+16)>0 && word(si+52)>0);
 for(unsigned i=56;i<64;i++)CHECK(si[i]==0);
 for(unsigned i=64;i<80;i++)CHECK(si[i]==0xa5);
 CHECK(call(116,1,0,0,0,0)==-14);
 int ts[2]={0,1000000};
 CHECK(call(267,1,0,(u32)ts,0,0)==0);
 ts[0]=0;ts[1]=0; CHECK(call(267,1,1,(u32)ts,0,0)==0);
 ts[1]=1000000000; CHECK(call(267,1,0,(u32)ts,0,0)==-22);
 CHECK(call(267,1,0,1,0,0)==-14);
 ts[1]=0; CHECK(call(267,999,0,(u32)ts,0,0)==-22);
'''
base.CHILD=base.CHILD[:base.CHILD.index('void _start(void){')]+ 'void _start(void){\n'+extra+'\n finish(0);\n}\n'
if __name__=='__main__':
 run_probe('compat-memfd-boot',base.INIT.replace('COMPAT_STAT_BOOT','COMPAT_MEMFD_BOOT'),
           'COMPAT_MEMFD_BOOT: PASS',setup=base.setup,cpus=1,timeout=90)
