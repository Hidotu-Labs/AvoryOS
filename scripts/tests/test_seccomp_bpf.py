#!/usr/bin/env python3
"""Actual classic-BPF verifier/interpreter against libseccomp-generated filters."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
source = (ROOT / 'kernel/src/sched/seccomp.c').read_text()
def function(signature):
    a = source.index(signature)
    b = source.index('{', a)
    depth = 1
    i = b + 1
    while depth:
        if source[i] == '{': depth += 1
        elif source[i] == '}': depth -= 1
        i += 1
    return source[a:i]

code = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <seccomp.h>
#include <sys/syscall.h>
static int live,fail;
static void *kmalloc(size_t n) {if(fail)return NULL;void*p=malloc(n);if(p)live++;return p;}
static void kfree(void*p){if(p){live--;free(p);}}
'''
code += source[source.index('#define SC_ALLOW'):source.index('static void sc_get')]
code += function('static int sc_verify(')
code += function('static uint32_t sc_run(')
code += r'''
static struct seccomp_filter *program(const struct sc_instruction *p,unsigned n) {
 struct seccomp_filter *f=calloc(1,sizeof(*f)+8*n);assert(f);
 f->count=n;memcpy(f->insn,p,n*8);return f;
}
static void invalid(const struct sc_instruction *p,unsigned n) {
 struct seccomp_filter *f=program(p,n);assert(sc_verify(f)==-22);free(f);assert(!live);
}
int main(void) {
 struct sc_instruction oob[]={{0x20,0,0,64},{6,0,0,SC_ALLOW}};invalid(oob,2);
 oob[0].k=1;invalid(oob,2);
 struct sc_instruction div0[]={{0x34,0,0,0},{6,0,0,SC_ALLOW}};invalid(div0,2);
 struct sc_instruction jump[]={{5,0,0,1},{6,0,0,SC_ALLOW}};invalid(jump,2);
 struct sc_instruction scratch[]={{0x60,0,0,0},{6,0,0,SC_ALLOW}};invalid(scratch,2);
 struct sc_instruction joined[]={{0x20,0,0,0},{0x15,1,0,39},{2,0,0,0},
  {0x60,0,0,0},{6,0,0,SC_ALLOW}};invalid(joined,5);
 struct sc_instruction farjump[]={{5,0,0,0xffffffffU},{6,0,0,SC_ALLOW}};invalid(farjump,2);
 struct sc_instruction badcond[]={{0x15,255,0,0},{6,0,0,SC_ALLOW}};invalid(badcond,2);
 struct sc_instruction fallthrough[]={{0,0,0,0}};invalid(fallthrough,1);
 struct sc_instruction badstore[]={{2,0,0,16},{6,0,0,SC_ALLOW}};invalid(badstore,2);
 struct sc_instruction indirect[]={{0x40,0,0,0},{6,0,0,SC_ALLOW}};invalid(indirect,2);
 struct sc_instruction shift32[]={{0x64,0,0,32},{6,0,0,SC_ALLOW}};invalid(shift32,2);
 struct sc_instruction initialized[]={{0,0,0,7},{2,0,0,0},{0x60,0,0,0},{0x16,0,0,0}};
 struct seccomp_filter *f=program(initialized,4);assert(!sc_verify(f));
 struct sc_data d={0};assert(sc_run(f,&d)==7);
 fail=1;assert(sc_verify(f)==-12);fail=0;free(f);assert(!live);
 struct sc_instruction paths[]={{0x20,0,0,0},{0x15,0,3,39},{0,0,0,7},{2,0,0,0},
  {5,0,0,2},{0,0,0,9},{2,0,0,0},{0x60,0,0,0},{0x16,0,0,0}};
 f=program(paths,9);assert(!sc_verify(f));d.nr=39;assert(sc_run(f,&d)==7);
 d.nr=1;assert(sc_run(f,&d)==9);free(f);
 struct sc_instruction divx[]={{0,0,0,10},{1,0,0,0},{0x3c,0,0,0},{0x16,0,0,0}};
 f=program(divx,4);assert(!sc_verify(f));assert(sc_run(f,&d)==0);
 f->insn[1].k=3;assert(sc_run(f,&d)==3);free(f);
 struct sc_instruction modx[]={{0,0,0,10},{1,0,0,0},{0x9c,0,0,0},{0x16,0,0,0}};
 f=program(modx,4);assert(!sc_verify(f));assert(sc_run(f,&d)==0);
 f->insn[1].k=3;assert(sc_run(f,&d)==1);free(f);

 scmp_filter_ctx ctx=seccomp_init(SCMP_ACT_ALLOW);assert(ctx);
 assert(!seccomp_rule_add(ctx,SCMP_ACT_ERRNO(EACCES),SCMP_SYS(getpid),0));
 assert(!seccomp_rule_add(ctx,SCMP_ACT_ERRNO(123),SCMP_SYS(ioctl),1,
                         SCMP_A1(SCMP_CMP_EQ,0x123456789abcdef0ULL)));
 assert(!seccomp_rule_add(ctx,SCMP_ACT_ERRNO(42),SCMP_SYS(openat),1,
                         SCMP_A2(SCMP_CMP_MASKED_EQ,0x300,0x200)));
 FILE *file=tmpfile();assert(file);assert(!seccomp_export_bpf(ctx,fileno(file)));
 long size=ftell(file);assert(size>0 && !(size%8));rewind(file);
 f=calloc(1,sizeof(*f)+(size_t)size);assert(f);f->count=size/8;
 assert(fread(f->insn,1,size,file)==(size_t)size);fclose(file);
 assert(!sc_verify(f));
 d=(struct sc_data){.nr=SYS_getpid,.arch=0xc000003e};
 assert(sc_run(f,&d)==(SC_ERRNO|EACCES));
 d.nr=SYS_write;assert(sc_run(f,&d)==SC_ALLOW);
 d.nr=SYS_ioctl;d.args[1]=0x123456789abcdef0ULL;assert(sc_run(f,&d)==(SC_ERRNO|123));
 d.args[1]^=1;assert(sc_run(f,&d)==SC_ALLOW);
 d.args[1]^=1;d.args[1]^=1ULL<<50;assert(sc_run(f,&d)==SC_ALLOW);
 d.nr=SYS_openat;d.args[2]=0x200;assert(sc_run(f,&d)==(SC_ERRNO|42));
 d.args[2]=0x300;assert(sc_run(f,&d)==SC_ALLOW);
 d.nr=SYS_getpid;d.arch=0x40000003;assert((sc_run(f,&d)&0xffff0000U)==0);
 d.arch=0xc000003e;d.nr=0x40000000U|SYS_getpid;
 assert((sc_run(f,&d)&0xffff0000U)==0); /* x32 is not a native syscall escape. */
 free(f);seccomp_release(ctx);assert(!live);
 puts("PASS: BPF validation, scratch joins, OOM and real libseccomp arch/64-bit argument filters");
}
'''
with tempfile.TemporaryDirectory(prefix='avory-seccomp-') as temp:
    src, exe = Path(temp) / 'test.c', Path(temp) / 'test'
    src.write_text(code)
    subprocess.run(['cc', '-std=gnu11', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-g', src, '-lseccomp', '-o', exe], check=True)
    subprocess.run([exe], check=True)
