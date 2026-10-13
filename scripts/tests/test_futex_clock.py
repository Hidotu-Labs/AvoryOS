#!/usr/bin/env python3
"""Test the actual futex clock conversion with a fixed boot epoch."""
from pathlib import Path
import os
import subprocess
import tempfile
s=Path('kernel/src/syscalls/sys_futex.c').read_text()
a=s.index('static int futex_wait_deadline(')
b=s.index('\nstatic uint64_t futex_wait(',a)
prefix='''#include <stdint.h>
#include <stdbool.h>
#include <assert.h>
#include <stdio.h>
#define EINVAL 22
uint64_t rtc_get_boot_timestamp(void) {return 1791393000ULL;}
'''
test='''
int main(void) {
 uint64_t d, t[2]={1791393060ULL,30000000ULL};
 assert(!futex_wait_deadline(t,true,true,60000,&d) && d==60030);
 t[0]=60; assert(!futex_wait_deadline(t,true,false,60000,&d) && d==60030);
 t[0]=0; assert(!futex_wait_deadline(t,false,false,60000,&d) && d==60030);
 t[1]=1; assert(!futex_wait_deadline(t,false,false,60000,&d) && d==60001);
 t[0]=1791392999ULL; t[1]=0;
 assert(!futex_wait_deadline(t,true,true,60000,&d) && d==0);
 t[0]=UINT64_MAX; assert(futex_wait_deadline(t,true,true,60000,&d)==-EINVAL);
 t[0]=0;t[1]=1000000000;assert(futex_wait_deadline(t,false,false,60000,&d)==-EINVAL);
 t[0]=INT64_MAX;t[1]=0;assert(!futex_wait_deadline(t,false,false,60000,&d) && d==UINT64_MAX);
 puts("PASS: realtime epoch conversion, monotonic/relative deadlines, sub-ms rounding, expiration, validation and overflow");
}
'''
with tempfile.TemporaryDirectory() as tmp:
 p=Path(tmp); (p/'test.c').write_text(prefix+s[a:b]+test)
 subprocess.run(['cc','-O1','-g','-fsanitize=address,undefined',str(p/'test.c'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True,env={**os.environ,'ASAN_OPTIONS':'detect_leaks=0'})
