#!/usr/bin/env python3
"""Run the real ELF initial-stack builder and i386 argument translator on host."""
from pathlib import Path
import subprocess
import tempfile
root = Path(__file__).resolve().parents[2]
source = (root / 'kernel/src/sched/process.c').read_text()
# Bound by the next function, excluding its descriptive comment.
begin=source.index('uint64_t process_build_initial_stack(')
opening=source.index('{',begin); depth=1; end=opening+1
while depth:
    depth+=(source[end]=='{')-(source[end]=='}'); end+=1
stack=source[begin:end]
writer=source[source.index('struct process_stack_writer {'):source.index('// Removed process_return_ctx')]
compat=(root/'kernel/src/syscalls/sys_compat.c').read_text()
compat=compat[compat.index('static int compat_syscall_number('):compat.index('\nvoid syscall_compat_dispatch(')]
prefix=r'''
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include "kernel/src/sched/elf.h"
#include "kernel/src/syscalls/syscall.h"
#include "kernel/src/cpu/isr.h"
#define PAGE_SIZE 4096
#define TSC_BEGIN(tag) ((void)0)
#define TSC_END(tag) ((void)0)
#define VDSO_USER_BASE 0x7ffffffe0000ULL
struct mm_struct {
  uint64_t saved_auxv[64]; uint32_t auxv_count;
  char saved_cmdline[256]; uint32_t cmdline_len;
  uint64_t arg_start, arg_end;
};
struct thread { struct mm_struct *mm; };
static struct mm_struct mm;
static struct thread thread={&mm};
static struct thread *sched_get_current(void){return &thread;}
static uint64_t *vmm_get_active_pml4(void){return 0;}
static unsigned char memory[32768] __attribute__((aligned(4096)));
static uint64_t base;
static bool mapped[sizeof(memory)/PAGE_SIZE];
static unsigned walk_count, map_count, alloc_count, heap_count;
static uint64_t wanted_page;
#define PAGE_FLAG_USER 4
#define PAGE_FLAG_RW 2
#define PAGE_FLAG_PRESENT 1
static uint64_t pmm_get_hhdm_offset(void){return 0;}
static uint64_t vmm_virt_to_phys(uint64_t *pml4,uint64_t va){
  (void)pml4; assert(va>=base && va<base+sizeof(memory));
  walk_count++;wanted_page=va;
  return mapped[(va-base)/PAGE_SIZE] ? (uint64_t)(memory+va-base) : 0;
}
static void *pmm_alloc_zeroed_page(void){return NULL;}
static void *pmm_alloc(void){
  assert(alloc_count<sizeof(memory)/PAGE_SIZE);
  alloc_count++;return memory+wanted_page-base;
}
static void pmm_free(void *page){(void)page;assert(0);}
static bool vmm_map_page(uint64_t *pml4,uint64_t va,uint64_t phys,uint64_t flags){
  (void)pml4;(void)flags;
  assert(phys==(uint64_t)(memory+va-base));
  assert(!mapped[(va-base)/PAGE_SIZE]);mapped[(va-base)/PAGE_SIZE]=true;
  map_count++;return true;
}
static bool proc_dump_enabled(void){return false;}
static void klog_puts(const char *s){(void)s;}
static void klog_uint64(uint64_t n){(void)n;}
static void *kmalloc(size_t n){heap_count++;return malloc(n);}
#define kfree free
static int copy_from_user(void *dst,const void *src,size_t n){
  if(!src)return -1;
  assert((uintptr_t)src>=base && (uintptr_t)src+n<=base+sizeof(memory));
  memcpy(dst,memory+(uintptr_t)src-base,n);return 0;
}
_Static_assert(offsetof(struct syscall_regs,user_cs)==128,"assembly CS offset");
_Static_assert(offsetof(struct syscall_regs,user_rcx)==136,"assembly ECX offset");
_Static_assert(offsetof(struct syscall_regs,user_rsi)==144,"assembly ESI offset");
_Static_assert(offsetof(struct syscall_regs,user_rdi)==152,"assembly EDI offset");
'''
tests=r'''
static uint64_t word(uint64_t va,bool narrow){
  uint64_t v=0;memcpy(&v,memory+va-base,narrow?4:8);return v;
}
static void check_stack(bool narrow){
  const char *args[]={"/bin/test32","second",NULL}, *env[]={"HELLO=world",NULL};
  uint64_t top=narrow?ELF32_STACK_TOP:0x7fffff000000ULL;
  base=top-sizeof(memory);memset(memory,0xcc,sizeof(memory));
  memset(mapped,0,sizeof(mapped));walk_count=map_count=alloc_count=heap_count=0;
  elf_info_t info={.compat32=narrow,.entry=0x8048000,.phdr=0x8048034,
                  .phentsize=narrow?32:56,.phnum=2};
  uint64_t sp=process_build_initial_stack(top,args[0],args,env,&info);
  size_t size=narrow?4:8;assert(!(sp&15));assert(word(sp,narrow)==2);
  assert(!strcmp((char*)memory+word(sp+size,narrow)-base,args[0]));
  assert(!strcmp((char*)memory+word(sp+size*2,narrow)-base,args[1]));
  assert(word(sp+size*3,narrow)==0);
  assert(!strcmp((char*)memory+word(sp+size*4,narrow)-base,env[0]));
  assert(word(sp+size*5,narrow)==0);
  bool platform=false,phent=false,vdso=false,random=false;
  for(uint64_t a=sp+size*6;;a+=size*2){
    uint64_t type=word(a,narrow),value=word(a+size,narrow);
    if(type==AT_NULL)break;
    if(type==AT_PLATFORM){platform=true;
      assert(!strcmp((char*)memory+value-base,narrow?"i686":"x86_64"));}
    if(type==AT_PHENT){phent=true;assert(value==(narrow?32:56));}
    if(type==AT_SYSINFO_EHDR){vdso=true;assert(value==(narrow?0:VDSO_USER_BASE));}
    if(type==AT_RANDOM){random=true;assert(value>=base && value+16<=top);}
  }
  assert(platform&&phent&&vdso&&random);
  assert(!memcmp(mm.saved_cmdline,"/bin/test32\0second\0",19));
  assert(heap_count==0 && map_count==1 && alloc_count==1 && walk_count==1);
  assert(memory[sizeof(memory)-1]==0); // Unwritten bytes of new page are zero.
} 
static void check_large_stack(bool narrow){
  char text[6000];memset(text,'x',sizeof(text));text[sizeof(text)-1]=0;
  const char *args[]={"test",NULL};
  const char *env[151];for(int i=0;i<149;i++)env[i]="A=B";
  env[149]=text;env[150]=NULL;
  uint64_t top=narrow?ELF32_STACK_TOP:0x7fffff000000ULL;
  base=top-sizeof(memory);memset(memory,0xcc,sizeof(memory));
  memset(mapped,0,sizeof(mapped));walk_count=map_count=alloc_count=heap_count=0;
  elf_info_t info={.compat32=narrow};
  uint64_t sp=process_build_initial_stack(top,args[0],args,env,&info);
  size_t size=narrow?4:8;assert(sp && !(sp&15));assert(word(sp,narrow)==1);
  for(int i=0;i<150;i++)
    assert(!strcmp((char*)memory+word(sp+size*(3+i),narrow)-base,env[i]));
  assert(word(sp+size*153,narrow)==0);
  assert(heap_count==1 && map_count==(narrow?2:3));
  assert(alloc_count==map_count && walk_count==(narrow?3:4));
  // Lower untouched pages retain poison: the builder mapped only written pages.
  assert(memory[0]==0xcc);
}
static void check_empty_and_defaults(void){
  const char *empty[]={NULL};
  uint64_t top=0x7fffff000000ULL;
  base=top-sizeof(memory);memset(memory,0xcc,sizeof(memory));
  memset(mapped,0,sizeof(mapped));walk_count=map_count=alloc_count=heap_count=0;
  elf_info_t info={0};
  uint64_t sp=process_build_initial_stack(top,"test",empty,empty,&info);
  assert(word(sp,false)==0 && word(sp+8,false)==0 && word(sp+16,false)==0);
  bool execfn=false;
  for(uint64_t a=sp+24;word(a,false)!=AT_NULL;a+=16)
    if(word(a,false)==AT_EXECFN){execfn=true;assert(word(a+8,false)==0);}
  assert(execfn && mm.cmdline_len==0 && mm.arg_start==0);
  base=top-sizeof(memory);memset(memory,0xcc,sizeof(memory));
  memset(mapped,0,sizeof(mapped));walk_count=map_count=alloc_count=heap_count=0;
  sp=process_build_initial_stack(top,"test",NULL,NULL,NULL);
  assert(word(sp,false)==1);
  assert(!strcmp((char*)memory+word(sp+8,false)-base,"test"));
  assert(!strncmp((char*)memory+word(sp+24,false)-base,"PATH=",5));
  assert(heap_count==0 && map_count==1 && walk_count==1);
}
int main(void){
  check_empty_and_defaults();
  check_stack(true);check_stack(false);
  check_large_stack(true);check_large_stack(false);
  struct registers r={.rax=4,.rbx=1,.rcx=0x12345678,.rdx=9,
      .rsi=0xabcdef,.rdi=0x76543210,.rbp=0x13572468,.cs=0x1b,
      .rip=0x8048100,.rsp=0xbffff000,.rflags=0x202};
  struct syscall_regs s;assert(compat_prepare(&r,&s)==0);
  assert(s.rax==SYS_WRITE && s.rdi==1 && s.rsi==r.rcx && s.rdx==9);
  assert(s.user_rcx==r.rcx && s.user_rsi==r.rsi && s.user_rdi==r.rdi);
  assert(s.user_cs==0x1b && s.rbp==r.rbp && s.rbx==r.rbx);
  r.rax=2;assert(compat_prepare(&r,&s)==0 && s.rax==SYS_FORK);
  r.rax=192;r.rbp=3;r.rdi=0xffffffff;
  assert(!compat_prepare(&r,&s) && s.r9==12288 && s.r8==UINT64_MAX);
  r.rbp=0x100000;assert(!compat_prepare(&r,&s) && s.r9==0x100000000ULL);
  r.rbp=0xffffffff;assert(!compat_prepare(&r,&s) && s.r9==0xffffffff000ULL);
  r.rax=180;r.rsi=0x12345678;r.rdi=1;
  assert(!compat_prepare(&r,&s) && s.r10==0x112345678ULL);
  r.rax=37;r.rbx=0xffffffff;
  assert(!compat_prepare(&r,&s) && s.rdi==UINT64_MAX);
  r.rax=7;assert(!compat_prepare(&r,&s) && s.rdi==UINT64_MAX && s.r10==0);
  r.rax=120;r.rbx=CLONE_SETTLS;r.rsi=0x1234;assert(!compat_prepare(&r,&s) && s.r8==0x1234);
  r.rax=55;r.rcx=6;assert(compat_prepare(&r,&s)==0 && s.rax==SYS_FCNTL && s.rsi==6);
  r.rax=174;assert(!compat_prepare(&r,&s) && s.rax==SYS_RT_SIGACTION);
  r.rax=197;assert(!compat_prepare(&r,&s) && s.rax==SYS_FSTAT);
  r.rax=300;r.rbx=(uint32_t)-100;r.rcx=0x1234;r.rdx=0x5678;r.rsi=0x100;
  assert(!compat_prepare(&r,&s) && s.rax==SYS_NEWFSTATAT);
  assert((int)s.rdi==-100 && s.rsi==0x1234 && s.rdx==0x5678 && s.r10==0x100);
  r.rax=383;r.rdi=0x9876;
  assert(!compat_prepare(&r,&s) && s.rax==SYS_STATX && s.r8==0x9876);
  r.rax=90;r.rbx=0;assert(compat_prepare(&r,&s)==-14);
  base=0x10000;uint32_t args[]={0,4096,3,0x22,0xffffffff,8192};
  memcpy(memory,args,sizeof(args));r.rbx=base;
  assert(!compat_prepare(&r,&s));assert(s.rsi==4096 && s.r9==8192);
}
'''
with tempfile.TemporaryDirectory(prefix='avory-elf32-') as tmp:
    path=Path(tmp);(path/'test.c').write_text(prefix+writer+stack+compat+tests)
    subprocess.run(['cc','-std=gnu11','-Wall','-Wextra','-Werror',
                    '-fsanitize=undefined','-I',str(root),str(path/'test.c'),
                    '-o',str(path/'test')],check=True)
    subprocess.run([str(path/'test')],check=True)
print('PASS: ELF32/ELF64 startup stacks, i386 register ABI, offsets and layout guards')
