#!/usr/bin/env python3
"""Exercise the kernel ELF loader with host VFS/VMA mocks and real ELF headers."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
source = (ROOT / 'kernel/src/sched/process.c').read_text()
loader = source[source.index('static bool elf_read_phdr('):source.index('\nbool elf_load(')]
constants = source[source.index('#define PAGE_SIZE '):source.index('// Permissions are applied lazily:')]
prefix = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "kernel/src/sched/elf.h"
#define MAP_PRIVATE 2
#define TSC_BEGIN(x) ((void)0)
#define TSC_END(x) ((void)0)
typedef struct { FILE *stream; uint64_t length; } vfs_node_t;
struct region { uint64_t start, end, prot; void *file; };
struct vma_list { struct region regions[32]; int count; };
struct mm { struct vma_list vmas; uint64_t brk_base, brk_current; };
struct thread { struct mm *mm; vfs_node_t *cwd_node; };
static struct mm mm;
static vfs_node_t root_node, cwd_node;
static vfs_node_t *fs_root=&root_node;
static struct thread thread = { .mm=&mm, .cwd_node=&cwd_node };
static vfs_node_t node;
static bool fail_alloc;
static struct thread *sched_get_current(void) { return &thread; }
static vfs_node_t *vfs_resolve_path_at(vfs_node_t *base, const char *path) {
  assert(base == (path[0] == '/' ? fs_root : thread.cwd_node ? thread.cwd_node : fs_root));
  node.stream = fopen(path, "rb");
  if (!node.stream) return NULL;
  fseek(node.stream, 0, SEEK_END); node.length=ftell(node.stream);
  return &node;
}
static uint32_t vfs_read(vfs_node_t *n, uint32_t off, uint32_t len, uint8_t *buf) {
  assert(!fseek(n->stream, off, SEEK_SET)); return fread(buf, 1, len, n->stream);
}
static void vfs_close(vfs_node_t *n) { assert(!fclose(n->stream)); }
static void *kmalloc(size_t n) { return fail_alloc ? NULL : malloc(n); }
static void kfree(void *p) { free(p); }
static void klog_puts(const char *s) { (void)s; }
static int vma_add(struct vma_list *v, uint64_t start, uint64_t end,
                   uint64_t prot, uint64_t flags, int fd, uint64_t offset,
                   void *file, uint64_t size) {
  (void)flags; (void)fd; (void)offset; (void)file; (void)size;
  assert(v->count < 32); v->regions[v->count++] = (struct region){start,end,prot,file};
  return 0;
}
'''
tests = r'''
static void fixture(const char *path, int type, bool interp, bool unusual) {
  Elf64_Ehdr e = { .e_ident = {0x7f,'E','L','F',2,1,1}, .e_type = type,
    .e_machine=62, .e_version=1, .e_ehsize=sizeof(e),
    .e_entry = 0x1108, .e_phoff = sizeof(e),
    .e_phentsize = sizeof(Elf64_Phdr) + (unusual ? 8 : 0),
    .e_phnum = interp ? 2 : 1 };
  Elf64_Phdr p = { .p_type = PT_LOAD, .p_vaddr = 0,
    .p_filesz = 0x200, .p_memsz = 0x6000, .p_flags = PF_R|PF_X };
  FILE *f = fopen(path, "wb"); assert(f);
  assert(fwrite(&e, sizeof(e), 1, f) == 1);
  assert(fwrite(&p, sizeof(p), 1, f) == 1);
  if (unusual) { uint64_t pad = 0; fwrite(&pad, sizeof(pad), 1, f); }
  if (interp) { // PT_INTERP deliberately follows PT_LOAD.
    p = (Elf64_Phdr){ .p_type = PT_INTERP,
      .p_offset = sizeof(e) + e.e_phnum * e.e_phentsize, .p_filesz = 10 };
    fwrite(&p, sizeof(p), 1, f);
    if (unusual) { uint64_t pad = 0; fwrite(&pad, sizeof(pad), 1, f); }
    fwrite("/ld.so\0\0\0", 10, 1, f);
  }
  fseek(f, 0x1ff, SEEK_SET); fputc(0, f);
  assert(!fclose(f));
}
static void check(const char *path, uint64_t requested, bool is_interp,
                   uint64_t expected, bool low_reservation) {
  memset(&mm, 0, sizeof(mm)); elf_info_t info = {0}; char interp[64] = {0};
  assert(do_elf_load(path, NULL, requested, is_interp, &info, interp, sizeof(interp)));
  assert(mm.vmas.count);
  uint64_t base = is_interp ? info.interp_base : info.load_base;
  uint64_t entry = is_interp ? info.interp_entry : info.entry;
  assert(base == expected);
  assert(entry == expected + 0x1108);
  if (!is_interp) {
    assert(info.phdr == expected + sizeof(Elf64_Ehdr));
    assert(mm.brk_current >= expected + 0x6000);
  }
  if (low_reservation) {
    // Wine's MAP_FIXED PROT_NONE reservations must not remove any image page.
    for (int i = 0; i < mm.vmas.count; i++)
      assert(mm.vmas.regions[i].end <= 0x10000 ||
             mm.vmas.regions[i].start >= 0x68000000);
  }
}

static void fixture32(const char *path, int type, bool interp) {
  Elf32_Ehdr e = { .e_ident={0x7f,'E','L','F',1,1,1}, .e_type=type,
      .e_machine=3, .e_version=1, .e_ehsize=sizeof(e), .e_entry=0x108,
      .e_phoff=sizeof(e), .e_phentsize=sizeof(Elf32_Phdr),
      .e_phnum=interp ? 2 : 1 };
  Elf32_Phdr p = { .p_type=PT_LOAD, .p_filesz=0x200, .p_memsz=0x6000,
                   .p_flags=PF_R|PF_X };
  FILE *f=fopen(path,"wb"); assert(f);
  fwrite(&e,sizeof(e),1,f); fwrite(&p,sizeof(p),1,f);
  if(interp) {
    p=(Elf32_Phdr){.p_type=PT_INTERP,
                   .p_offset=sizeof(e)+2*sizeof(p),.p_filesz=10};
    fwrite(&p,sizeof(p),1,f); fwrite("/ld.so\0\0\0",10,1,f);
  }
  fseek(f,0x1ff,SEEK_SET);fputc(0,f);fclose(f);
}
static void check32(const char *path, bool interp) {
  memset(&mm,0,sizeof(mm)); elf_info_t info={0};char name[64]={0};
  assert(do_elf_load(path,NULL,0,false,&info,name,sizeof(name)));
  assert(info.compat32 && info.phentsize==sizeof(Elf32_Phdr));
  assert(info.load_base==(interp?ELF_PIE_BASE:ELF32_INTERP_BASE));
  assert(info.entry==info.load_base+0x108);
  assert(info.phdr==info.load_base+sizeof(Elf32_Ehdr));
  assert(mm.vmas.regions[0].end < ELF32_USER_LIMIT);
  if(interp)assert(!strcmp(name,"/ld.so"));
  memset(&mm,0,sizeof(mm));info=(elf_info_t){0};
  assert(do_elf_load(path,NULL,ELF_INTERP_BASE,true,&info,NULL,0));
  assert(info.interp_base==ELF32_INTERP_BASE);
}
static void invalid32(const char *path) {
  fixture32(path,ET_DYN,false);
  FILE *f=fopen(path,"r+b");Elf32_Ehdr e;fread(&e,sizeof(e),1,f);
  e.e_machine=40;rewind(f);fwrite(&e,sizeof(e),1,f);fclose(f);
  elf_info_t info={0};assert(!do_elf_load(path,NULL,0,false,&info,NULL,0));
  fixture32(path,ET_DYN,false);
  f=fopen(path,"r+b");Elf32_Phdr p;fseek(f,sizeof(e),SEEK_SET);
  fread(&p,sizeof(p),1,f);p.p_vaddr=0xbffff000;p.p_memsz=0x6000;
  fseek(f,sizeof(e),SEEK_SET);fwrite(&p,sizeof(p),1,f);fclose(f);
  assert(!do_elf_load(path,NULL,0,false,&info,NULL,0));
  fixture32(path,ET_DYN,false);
  f=fopen(path,"r+b");fseek(f,sizeof(e),SEEK_SET);fread(&p,sizeof(p),1,f);
  p.p_filesz=0x7000;fseek(f,sizeof(e),SEEK_SET);fwrite(&p,sizeof(p),1,f);fclose(f);
  assert(!do_elf_load(path,NULL,0,false,&info,NULL,0));
}


static void pure_bss32(const char *path) {
  fixture32(path,ET_DYN,false);
  FILE *f=fopen(path,"r+b");Elf32_Ehdr e;fread(&e,sizeof(e),1,f);
  e.e_phnum=2;rewind(f);fwrite(&e,sizeof(e),1,f);
  Elf32_Phdr p={.p_type=PT_LOAD,.p_vaddr=0x9000,.p_memsz=4096,
                .p_flags=PF_R|PF_W};
  fseek(f,sizeof(e)+sizeof(p),SEEK_SET);fwrite(&p,sizeof(p),1,f);fclose(f);
  memset(&mm,0,sizeof(mm));elf_info_t info={0};
  assert(do_elf_load(path,NULL,0,false,&info,NULL,0));
  assert(mm.vmas.count==2 && mm.vmas.regions[0].file);
  assert(!mm.vmas.regions[1].file && mm.vmas.regions[1].prot==3);
}
int main(int argc, char **argv) {
  assert(argc >= 2);
  for (int fallback = 0; fallback < 2; fallback++) {
    fail_alloc = fallback;
    for (int unusual = 0; unusual < 2; unusual++) {
      fixture(argv[1], ET_DYN, false, unusual);
      check(argv[1], 0, false, ELF_INTERP_BASE, true);
      check(argv[1], ELF_INTERP_BASE, true, ELF_INTERP_BASE, true);
      check(argv[1], 0x100000000ULL, false, 0x100000000ULL, true);
      fixture(argv[1], ET_DYN, true, unusual);
      check(argv[1], 0, false, ELF_PIE_BASE, false);
      fixture(argv[1], ET_EXEC, false, unusual);
      check(argv[1], 0, false, 0, false);
    }
    fixture32(argv[1],ET_DYN,false); check32(argv[1],false);
    fixture32(argv[1],ET_DYN,true); check32(argv[1],true);
    invalid32(argv[1]); pure_bss32(argv[1]);
    if (argc > 2) {
      memset(&mm, 0, sizeof(mm)); elf_info_t info = {0};
      assert(do_elf_load(argv[2], NULL, 0, false, &info, NULL, 0));
      assert(info.entry == ELF_INTERP_BASE + 0x1000);
      assert(info.phdr == ELF_INTERP_BASE + 64);
      for (int i = 0; i < mm.vmas.count; i++)
        assert(mm.vmas.regions[i].start >= 0x68000000);
      assert(info.entry != 0x401000);
    }
  }
  if(argc > 3) {
    memset(&mm,0,sizeof(mm));elf_info_t info={0};
    assert(do_elf_load(argv[3],NULL,0,false,&info,NULL,0));
    assert(info.compat32 && info.load_base==0 && info.entry<ELF32_USER_LIMIT);
    assert(mm.vmas.count>=3);
  }
  /* The ELF read must resolve against cwd, not the global root. */
  fixture(argv[1], ET_DYN, false, false);
  char *oldcwd=getcwd(NULL,0), *dir=strdup(argv[1]);assert(oldcwd&&dir);
  char *leaf=strrchr(dir,'/');assert(leaf);*leaf++=0;assert(!chdir(dir));
  check(leaf,0,false,ELF_INTERP_BASE,true);
  thread.cwd_node=NULL;check(leaf,0,false,ELF_INTERP_BASE,true);
  thread.cwd_node=&cwd_node;assert(!chdir(oldcwd));free(dir);free(oldcwd);
  puts("PASS: self-loader placement, late PT_INTERP, ET_EXEC, explicit bias, header read fallbacks, Wine image, ELF32 placement/rejection and smoke BSS");
}
'''
with tempfile.TemporaryDirectory() as tmp:
    tmp = Path(tmp)
    (tmp / 'test.c').write_text(prefix + constants + loader + tests)
    subprocess.run(['cc', '-std=gnu11', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=undefined', '-I', str(ROOT),
                    str(tmp / 'test.c'), '-o', str(tmp / 'test')], check=True)
    args = [str(tmp / 'test'), str(tmp / 'fixture')]
    wine = ROOT / 'build/arch/rootfs/usr/lib/wine/x86_64-unix/wine-preloader'
    if wine.exists():
        args.append(str(wine))
        smoke = ROOT / 'userland/test_elf32.elf'
        if smoke.exists(): args.append(str(smoke))
    subprocess.run(args, check=True)
