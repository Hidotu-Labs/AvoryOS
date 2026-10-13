#!/usr/bin/env python3
"""Check the actual join candidate filter against reported false positives."""
from pathlib import Path
import os, subprocess, tempfile
source=Path('kernel/src/syscalls/sysdiag.c').read_text()
def extract(signature):
 a=source.index(signature);b=source.index('{',a)+1;depth=1
 while depth:
  depth+=(source[b]=='{')-(source[b]=='}');b+=1
 return source[a:b]+'\n'
code=r'''
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
typedef struct {bool used;char comm[32];uint64_t syscall_num;int64_t error_code;} sysdiag_syscall_error_t;
'''
code+=extract('static bool sysdiag_comm_is_game(')
code+=extract('static bool sysdiag_err_is_join_signature(')
code+=r'''
typedef struct {int owner;} ext3_journal_state_t;
typedef struct {ext3_journal_state_t journal;} ext2_mount_t;
struct node {void *device; int (*statfs)(void *,void *);};
static struct node *fs_root;
static int ext2_statfs_impl(void *n,void *b){(void)n;(void)b;return 0;}
static int ext4_statfs_impl(void *n,void *b){(void)n;(void)b;return 0;}
'''
locksource=Path('kernel/src/lock/lockdiag.c').read_text()
start=locksource.index('static ext3_journal_state_t *lockdiag_root_journal(')
end=locksource.index('\n}',start)+2
code+=locksource[start:end]+'\n'
code+=r'''
int main(void){
 assert(!lockdiag_root_journal());
 ext2_mount_t mount={0};struct node root={.device=&mount,.statfs=ext4_statfs_impl};fs_root=&root;
 assert(lockdiag_root_journal()==&mount.journal);
 root.statfs=ext2_statfs_impl;assert(lockdiag_root_journal()==&mount.journal);
 root.statfs=NULL;assert(!lockdiag_root_journal());
 root.statfs=ext4_statfs_impl;root.device=NULL;assert(!lockdiag_root_journal());
 puts("PASS: journal spotlight recognizes root directories and rejects unrelated/missing backends");
 sysdiag_syscall_error_t e={.used=true,.syscall_num=202,.error_code=-110};
 const char *workers[]={"pool-0","blocking-8","RBX Worker A","Main","mocktail","Wine","sober","async-io"};
 for(unsigned i=0;i<sizeof(workers)/sizeof(*workers);i++){
  strcpy(e.comm,workers[i]);assert(!sysdiag_err_is_join_signature(&e));
  e.error_code=-11;assert(!sysdiag_err_is_join_signature(&e));
  e.error_code=-4;assert(!sysdiag_err_is_join_signature(&e));e.error_code=-110;
 }
 strcpy(e.comm,"mocktail");e.syscall_num=47;e.error_code=-11;
 assert(!sysdiag_err_is_join_signature(&e));
 e.error_code=-14;assert(sysdiag_err_is_join_signature(&e));
 e.syscall_num=42;e.error_code=-110;strcpy(e.comm,"HttpClient");
 assert(sysdiag_err_is_join_signature(&e));
 int errors[]={5,28,18,32};
 for(unsigned i=0;i<4;i++){e.error_code=-errors[i];assert(sysdiag_err_is_join_signature(&e));}
 e.used=false;assert(!sysdiag_err_is_join_signature(&e));
 assert(!sysdiag_err_is_join_signature(NULL));
 puts("PASS: worker futex timeouts and EAGAIN/EINTR excluded; real I/O/compatibility candidates retained");
}
'''
with tempfile.TemporaryDirectory() as tmp:
 p=Path(tmp);(p/'test.c').write_text(code)
 subprocess.run(['cc','-O1','-g','-fsanitize=address,undefined',str(p/'test.c'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True,env={**os.environ,'ASAN_OPTIONS':'detect_leaks=0'})
