#!/usr/bin/env python3
"""Exercise the actual bounded parked-process reader without guest memory access."""
from pathlib import Path
import os
import subprocess
import tempfile

source = Path("kernel/src/lock/lockdiag.c").read_text()
start = source.index("#define LOCKDIAG_USER_FRAMES")
end = source.index("/* Bounded sanitized path print", start)
helpers = source[start:end]
start = source.index("static void dump_fd_wait(")
end = source.index("\nstatic void dump_thread(", start)
fd_helper = source[start:end]
code = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define PAGE_FLAG_USER 4
#define PAGE_FLAG_PS 128
#define MAX_FDS 64
#define THREAD_BLOCKED 2
typedef struct {bool locked;} spinlock_t;
static bool spinlock_try_acquire(spinlock_t *s) {
  if(s->locked)return false; s->locked=true; return true;
}
static void spinlock_release(spinlock_t *s) {assert(s->locked);s->locked=false;}
typedef struct {char name[128];uint32_t inode,flags;bool alive;} vfs_node_t;
static bool vfs_node_is_alive(vfs_node_t *n){return n && n->alive;}
struct vma {uint64_t start,end,prot,offset;void *file_node;};
struct vma_list {struct vma regions[4];};
struct mm_struct {spinlock_t lock;struct vma_list vmas;};
struct fd_path {char value[256];};
struct fd_table {spinlock_t lock;vfs_node_t *fds[MAX_FDS];struct fd_path *fd_paths[MAX_FDS];uint64_t fd_flags[MAX_FDS];};
struct thread {struct mm_struct *mm;struct fd_table *files;uint64_t cr3;
 uint64_t last_syscall_user_rip,last_syscall_user_rsp,last_syscall_num,last_syscall_args[6],last_io_started_ms;
 bool in_syscall,in_futex_wait;int state;};
static struct vma *vma_find(struct vma_list *list,uint64_t addr){
 for(unsigned i=0;i<4;i++)if(addr>=list->regions[i].start && addr<list->regions[i].end)return &list->regions[i];
 return NULL;
}
static _Alignas(4096) uint8_t ram[8192];
static unsigned walks;static uint64_t last_cr3;
static bool page2_missing,bad_leaf,supervisor,huge;
static uint64_t vmm_debug_walk(uint64_t cr3,uint64_t addr,uint64_t e[4]){
 walks++;last_cr3=cr3;
 if(addr<0x10000 || addr>=0x12000 || (page2_missing && addr>=0x11000))return 0;
 for(int i=0;i<4;i++)e[i]=1|PAGE_FLAG_USER;
 if(supervisor)e[1]&=~PAGE_FLAG_USER;
 if(huge){e[2]|=PAGE_FLAG_PS;e[3]=0;}
 if(bad_leaf)return 0xdead000;
 return 0x1000+addr-0x10000;
}
static bool pmm_is_managed(uint64_t p){return p==0x1000 || p==0x2000;}
static uint64_t pmm_get_hhdm_offset(void){return (uint64_t)ram-0x1000;}
static char output[8192];
static uint64_t lapic_timer_get_ms(void){return 201000;}
static void ld_str(const char *s){assert(strlen(output)+strlen(s)<sizeof(output));strcat(output,s);}
static void ld_hex(uint64_t x){char b[32];snprintf(b,sizeof(b),"0x%llx",(unsigned long long)x);ld_str(b);}
static void ld_dec(uint64_t x){char b[32];snprintf(b,sizeof(b),"%llu",(unsigned long long)x);ld_str(b);}
'''
code += helpers + fd_helper
code += r'''
int main(void){
 vfs_node_t libc={.name="libc.so.6",.alive=true},roblox={.name="libroblox.so",.alive=true};
 struct mm_struct mm={.vmas.regions={
  {0x10000,0x12000,3,0,NULL},{0x400000,0x401000,5,0x8000,&libc},
  {0x500000,0x501000,5,0x200000,&roblox},{0x700000,0x701000,1,0,&roblox}}};
 struct thread t={.mm=&mm,.cr3=0x123000,.in_syscall=true,.in_futex_wait=true,
  .state=THREAD_BLOCKED,.last_syscall_user_rsp=0x10ff0,.last_syscall_user_rip=0x400123};
 uint64_t stack[]={0x400123,0x500111,0x700001,0xffffffff80000000ULL,0x500222};
 memcpy(ram+0xff0,stack,sizeof(stack));
 uint32_t words[]={2,0,1539,0};memcpy(ram,words,sizeof(words));t.last_syscall_args[0]=0x10000;
 struct lockdiag_user_wait s;
 assert(lockdiag_collect_user_wait(&t,&s));
 assert(last_cr3==t.cr3 && s.count==3 && s.word_count==4 && !mm.lock.locked);
 assert(s.frames[0].offset==0x8123 && !strcmp(s.frames[0].module,"libc.so.6"));
 assert(s.frames[1].offset==0x200111 && s.frames[1].slot==0x10ff8);
 assert(s.frames[2].offset==0x200222 && s.frames[2].slot==0x11010);
 assert(s.words[0]==2 && s.words[2]==1539);
 dump_user_wait(&t);assert(strstr(output,"libroblox.so file+0x200111"));
 assert(strstr(output,"stack scan, not an unwind"));
 puts("PASS: parked CR3, mapped executable candidates, file offsets, dedup and futex words");
 page2_missing=true;assert(lockdiag_collect_user_wait(&t,&s));assert(s.count==2);page2_missing=false;
 unsigned before=walks;mm.lock.locked=true;assert(!lockdiag_collect_user_wait(&t,&s));assert(walks==before);mm.lock.locked=false;
 t.state=1;assert(!lockdiag_collect_user_wait(&t,&s));t.state=THREAD_BLOCKED;
 t.in_syscall=false;assert(!lockdiag_collect_user_wait(&t,&s));t.in_syscall=true;
 uint64_t value;assert(!lockdiag_user_read(&t,0x10fff,&value,8));
 assert(!lockdiag_user_read(&t,0x800000000000ULL,&value,8));
 assert(!lockdiag_user_read(&t,0x12000,&value,8));
 supervisor=true;assert(!lockdiag_user_read(&t,0x10000,&value,8));supervisor=false;
 bad_leaf=true;assert(!lockdiag_user_read(&t,0x10000,&value,8));bad_leaf=false;
 huge=true;assert(lockdiag_user_read(&t,0x10000,&value,8));huge=false;
 puts("PASS: busy locks, unmapped/misaligned/kernel addresses, supervisor/MMIO leaves, huge pages");
 struct fd_path path={.value="/opt/mocktail/data/android/data/cache/db"};
 struct fd_table files={0};vfs_node_t db={.name="db",.alive=true,.inode=901};
 files.fds[34]=&db;files.fd_paths[34]=&path;t.files=&files;
 t.last_syscall_num=18;t.last_syscall_args[0]=34;t.last_syscall_args[2]=4096;t.last_syscall_args[3]=12288;
 output[0]=0;dump_fd_wait(&t);assert(strstr(output,"offset=12288 inode=901 path='/opt/mocktail/data/android/data/cache/db'"));
 files.fd_flags[34]=0x800;db.flags=0x25;output[0]=0;dump_fd_wait(&t);
 assert(strstr(output,"fd_flags=0x800 node_flags=0x25"));
 files.lock.locked=true;output[0]=0;dump_fd_wait(&t);assert(!strstr(output,"path="));files.lock.locked=false;
 path.value[0]=0;output[0]=0;dump_fd_wait(&t);assert(strstr(output,"path='db'"));
 t.last_syscall_args[0]=100;output[0]=0;dump_fd_wait(&t);assert(!strstr(output,"path="));
 puts("PASS: bounded file paths/inodes/offsets, busy descriptor tables and invalid fds");
}
'''
with tempfile.TemporaryDirectory(prefix="avory-user-wait-") as tmp:
    path = Path(tmp)
    (path / "test.c").write_text(code)
    subprocess.run(["cc", "-std=gnu11", "-O1", "-g", "-fsanitize=address,undefined",
                    str(path / "test.c"), "-o", str(path / "test")], check=True)
    subprocess.run([str(path / "test")], check=True,
                   env={**os.environ, "ASAN_OPTIONS": "detect_leaks=0"})
