#!/usr/bin/env python3
"""Run the real VMA clone walker with modeled PMM/page tables and OOM injection."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'kernel/src/mm/vmm_clone.c').read_text()
source = '\n'.join(line for line in source.splitlines() if not line.startswith('#include'))
fault = (root / 'kernel/src/mm/vmm_fault.c').read_text()
# Exercise the actual COW write logic too, including the sole-owner path.
begin = fault.index('    if (*pte & PAGE_FLAG_COW) {')
opening = fault.index('{', begin)
depth, end = 1, opening + 1
while depth:
    depth += (fault[end] == '{') - (fault[end] == '}')
    end += 1
cow = fault[begin:end]

prefix = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#define LOCK_SPINLOCK_H
typedef int spinlock_t;
#include "kernel/src/mm/vmm.h"
#undef vmm_lock_acquire
#include "kernel/src/mm/vma.h"
typedef struct vfs_node { bool mmap_shared_pmm_refs; } vfs_node_t;
#define PAGE_SIZE 4096
#define PHYS_TO_VIRT(p) ((void *)((uint64_t)(p) + pmm_get_hhdm_offset()))
#define MPOL_DEFAULT 0
struct frame { void *ptr; unsigned refs; } frames[4096];
static unsigned used, attempts, fail_at, increments, flushes;
static uint64_t kernel_root, special;
static uintptr_t pmm_get_hhdm_offset(void) { return 0; }
static struct frame *lookup(void *ptr) {
  for (unsigned i = 0; i < used; i++) if (frames[i].ptr == ptr) return &frames[i];
  assert(!"unknown frame"); return NULL;
}
static void *pmm_alloc(void) {
  if (++attempts == fail_at) return NULL;
  assert(used < 4096);
  void *ptr = aligned_alloc(4096, 4096); assert(ptr);
  memset(ptr, 0, 4096);
  frames[used++] = (struct frame){ptr, 1}; return ptr;
}
static bool pmm_is_managed(uint64_t phys) { return phys != special; }
static void pmm_incref(void *ptr) { lookup(ptr)->refs++; increments++; }
static void pmm_decref(void *ptr) { assert(lookup(ptr)->refs); lookup(ptr)->refs--; }
static void pmm_free_page(void *ptr) { pmm_decref(ptr); }
static void *pmm_alloc_huge_page(void) { return NULL; }
static void pmm_free(void *ptr) { pmm_decref(ptr); }
static uint16_t pmm_get_ref(void *ptr) { return lookup(ptr)->refs; }
static uint64_t pmm_get_zero_page_phys(void) { return 0; }
void vmm_lock_acquire_at_for(uint64_t *pml4, uint64_t va, uint64_t caller) {
  (void)pml4; (void)va; (void)caller;
}
static unsigned swap_refs[8];
static bool zram_slot_get(uint32_t slot) {assert(slot < 8 && swap_refs[slot]); swap_refs[slot]++; return true;}
void vmm_lock_release(void) {}
static void tlb_flush_deferred_context(uint64_t cr3) { (void)cr3; flushes++; }
uint64_t vmm_get_vsyscall_page_phys(void) { return special; }
uint64_t *vmm_get_kernel_pml4(void) { return (uint64_t *)kernel_root; }
struct vma *vma_find(struct vma_list *list, uint64_t va) {
  for (struct vma *v = list->root; v; v = v->right)
    if (va >= v->start && va < v->end) return v;
  return NULL;
}
static void release_table(uint64_t phys, int level, uint64_t base,
                          struct vma_list *vmas) {
  uint64_t *table = (void *)phys;
  for (unsigned i = 0; i < (level == 4 ? 256u : 512u); i++) {
    uint64_t entry = table[i];
    if (level == 1 && zram_pte_slot(entry)) {assert(swap_refs[zram_pte_slot(entry)]);swap_refs[zram_pte_slot(entry)]--;continue;}
    if (!(entry & PAGE_FLAG_PRESENT)) continue;
    uint64_t va = base | ((uint64_t)i << (12 + 9 * (level - 1)));
    assert(!(entry & PAGE_FLAG_PS));
    if (level > 1) release_table(entry & PAGE_MASK, level - 1, va, vmas);
    else {
      struct vma *v = vma_find(vmas, va);
      if ((entry & PAGE_MASK) != special &&
          !(v && (v->flags & MAP_SHARED) && !(v->flags & MAP_PAGECACHE)))
        pmm_decref((void *)(entry & PAGE_MASK));
    }
  }
  pmm_free_page((void *)phys);
}
void vmm_free_user_pages_vma(uint64_t phys, struct vma_list *vmas) {
  release_table(phys, 4, 0, vmas);
}
static unsigned live(void) {
  unsigned n = 0; for (unsigned i = 0; i < used; i++) n += frames[i].refs != 0;
  return n;
}
static void reset(void) {
  for (unsigned i = 0; i < used; i++) free(frames[i].ptr);
  used = attempts = fail_at = increments = flushes = 0;
  kernel_root = (uint64_t)pmm_alloc();
  ((uint64_t *)kernel_root)[256] = 0x12345003;
  special = (uint64_t)pmm_alloc();
}
static void map(uint64_t *root, uint64_t va, uint64_t phys, uint64_t flags) {
  uint64_t *table = root;
  for (int level = 4; level > 1; level--) {
    unsigned i = (va >> (12 + 9 * (level - 1))) & 511;
    if (!table[i]) table[i] = (uint64_t)pmm_alloc() | 7;
    table = (void *)(table[i] & PAGE_MASK);
  }
  table[(va >> 12) & 511] = phys | flags | PAGE_FLAG_PRESENT | PAGE_FLAG_USER;
}
static uint64_t *pte_at(uint64_t root, uint64_t va) {
  uint64_t *table = (void *)root;
  for (int level = 4; level > 1; level--) {
    uint64_t entry = table[(va >> (12 + 9 * (level - 1))) & 511];
    if (!(entry & PAGE_FLAG_PRESENT)) return NULL;
    table = (void *)(entry & PAGE_MASK);
  }
  return &table[(va >> 12) & 511];
}
static struct vma file_vma(uint64_t base, uint64_t length) {
  return (struct vma){.start = base, .end = base + length, .prot = 3,
    .flags = MAP_PRIVATE, .file_node = (void *)1, .file_size = length};
}
static bool vmm_zero_pool_usable(int policy) { (void)policy; return false; }
static void *pmm_alloc_zeroed_page(void) { return NULL; }
static void *vmm_alloc_page_numa(int policy, uint64_t mask, int node, uint64_t va) {
  (void)policy; (void)mask; (void)node; (void)va; return pmm_alloc();
}
static void page_zero(void *p) { memset(p, 0, 4096); }
static void page_copy(void *dst, void *src) { memcpy(dst, src, 4096); }
static void tlb_shootdown_page(uint64_t va) { (void)va; }
#define WALK_REJECT(...) return -1
#define WALK_UNLOCK() ((void)0)
static int break_cow(uint64_t *pte) {
  uint64_t virt = 0, cow_old_phys = 0;
  bool cow_broken = false;
  uint64_t pml4[512] = {0}, pdpt[512] = {0}, pd[512] = {0};
'''
prefix += cow + r'''
  assert(cow_broken);
  if (cow_old_phys) pmm_decref((void *)cow_old_phys);
  return 0;
}
'''

tests = r'''
static void compressed_fork(void) {
  reset();struct vma v={.start=0x400000,.end=0x801000,.flags=MAP_PRIVATE|MAP_ANONYMOUS};
  struct vma_list list={.root=&v};uint64_t *parent=pmm_alloc();
  map(parent,v.start,0,0);*pte_at((uint64_t)parent,v.start)=zram_pte(3);swap_refs[3]=1;
  map(parent,0x800000,(uint64_t)pmm_alloc(),PAGE_FLAG_RW);
  unsigned before=live();
  uint64_t child=vmm_clone_user_mappings_vma(parent,&list);assert(child&&swap_refs[3]==2);
  assert(*pte_at(child,v.start)==zram_pte(3));vmm_free_user_pages_vma(child,&list);
  assert(swap_refs[3]==1&&live()==before);
  // Fail each table allocation; partially inherited slot refs must unwind.
  for(unsigned failure=1;failure<=5;failure++){
    fail_at=attempts+failure;child=vmm_clone_user_mappings_vma(parent,&list);
    assert(!child&&swap_refs[3]==1&&live()==before);
  }
  fail_at=0;
  child=vmm_clone_user_mappings(parent);assert(child&&swap_refs[3]==2);
  vmm_free_user_pages_vma(child,&list);assert(swap_refs[3]==1);
}
static void all_clean(void) {
  reset();
  struct vma v = file_vma(0x400000, 64 * 4096);
  struct vma_list list = {.root = &v};
  uint64_t *parent = pmm_alloc();
  for (unsigned i = 0; i < 64; i++) {
    void *frame = pmm_alloc(); pmm_incref(frame); // cache + parent
    map(parent, v.start + i * 4096, (uint64_t)frame,
        PAGE_FLAG_COW | PAGE_FLAG_REFAULT);
  }
  unsigned before = live(), n = attempts;
  increments = 0;
  uint64_t child = vmm_clone_user_mappings_vma(parent, &list);
  assert(child && attempts - n == 3); // root, PDPT, PD; no leaf PT
  assert(increments == 0 && flushes == 0);
  for (unsigned i = 0; i < 64; i++) {
    assert(!pte_at(child, v.start + i * 4096));
    uint64_t entry = *pte_at((uint64_t)parent, v.start + i * 4096);
    assert(entry & PAGE_FLAG_REFAULT);
    assert(lookup((void *)(entry & PAGE_MASK))->refs == 2);
  }
  assert(((uint64_t *)child)[256] == ((uint64_t *)kernel_root)[256]);
  vmm_free_user_pages_vma(child, &list); assert(live() == before);
}
static void mixed_and_oom(unsigned fail) {
  reset();
  struct vma v = file_vma(0x400000, 3 * 4096);
  struct vma anon = {.start = 0x800000, .end = 0x801000,
                     .flags = MAP_PRIVATE | MAP_ANONYMOUS};
  v.right = &anon; struct vma_list list = {.root = &v};
  uint64_t *parent = pmm_alloc();
  void *clean = pmm_alloc(), *modified = pmm_alloc(), *tail = pmm_alloc();
  void *anonymous = pmm_alloc();
  memset(modified, 0xa5, 4096);
  map(parent, v.start, (uint64_t)clean, PAGE_FLAG_REFAULT | PAGE_FLAG_COW);
  map(parent, v.start + 4096, (uint64_t)modified, PAGE_FLAG_COW);
  map(parent, v.start + 8192, (uint64_t)tail, PAGE_FLAG_RW);
  map(parent, anon.start, (uint64_t)anonymous, PAGE_FLAG_RW);
  unsigned before = live(); fail_at = fail ? attempts + fail : 0;
  uint64_t child = vmm_clone_user_mappings_vma(parent, &list);
  if (fail && fail <= 5) {
    assert(!child && live() == before);
    assert(lookup(clean)->refs == 1 && lookup(modified)->refs == 1);
    assert(lookup(tail)->refs == 1 && lookup(anonymous)->refs == 1);
    return;
  }
  assert(child);
  assert(!*pte_at(child, v.start));
  assert((*pte_at(child, v.start + 4096) & PAGE_MASK) == (uint64_t)modified);
  assert(*(unsigned char *)(*pte_at(child, v.start + 4096) & PAGE_MASK) == 0xa5);
  assert(lookup(modified)->refs == 2);
  assert(*pte_at(child, anon.start) & PAGE_FLAG_COW);
  assert(!(*pte_at((uint64_t)parent, anon.start) & PAGE_FLAG_RW));
  vmm_free_user_pages_vma(child, &list); assert(live() == before);
}
static void exclusions(void) {
  reset();
  struct vma v = file_vma(0x400000, 4096);
  uint64_t marked = PAGE_FLAG_REFAULT | PAGE_FLAG_COW;
  assert(fork_can_refault(marked, &v, v.start));
  assert(fork_can_refault(marked | PAGE_FLAG_D, &v, v.start));
  assert(!fork_can_refault(PAGE_FLAG_COW, &v, v.start));
  assert(!fork_can_refault(marked | PAGE_FLAG_RW, &v, v.start));
  assert(!fork_can_refault(marked, NULL, v.start));
  v.file_size = 4095; assert(!fork_can_refault(marked, &v, v.start));
  v.file_size = 4096; v.offset = 1;
  assert(!fork_can_refault(marked, &v, v.start)); v.offset = 0;
  v.linux_vma = (void *)1; assert(!fork_can_refault(marked, &v, v.start));
  v.linux_vma = NULL; v.file_node = NULL;
  assert(!fork_can_refault(marked, &v, v.start)); v.file_node = (void *)1;
  uint64_t excluded[] = {MAP_SHARED, MAP_ANONYMOUS, MAP_SYSV_SHM};
  for (unsigned i = 0; i < 3; i++) {
    v.flags = MAP_PRIVATE | excluded[i];
    assert(!fork_can_refault(marked, &v, v.start));
  }
  // Shared cache references and special mappings still get inherited.
  v.flags = MAP_SHARED | MAP_PAGECACHE;
  struct vma_list list = {.root = &v}; uint64_t *parent = pmm_alloc();
  void *frame = pmm_alloc();
  map(parent, v.start, (uint64_t)frame, PAGE_FLAG_RW);
  map(parent, v.start + 4096, special, PAGE_FLAG_REFAULT);
  uint64_t child = vmm_clone_user_mappings_vma(parent, &list); assert(child);
  assert(lookup(frame)->refs == 2);
  assert((*pte_at(child, v.start + 4096) & PAGE_MASK) == special);
  vmm_free_user_pages_vma(child, &list); assert(lookup(frame)->refs == 1);
}
static void cow_provenance(void) {
  for (unsigned owners = 1; owners <= 2; owners++) {
    reset(); void *original = pmm_alloc(); memset(original, 0xa5, 4096);
    if (owners == 2) pmm_incref(original);
    uint64_t pte = (uint64_t)original | PAGE_FLAG_PRESENT | PAGE_FLAG_USER |
                   PAGE_FLAG_COW | PAGE_FLAG_REFAULT;
    assert(break_cow(&pte) == 0);
    assert(!(pte & (PAGE_FLAG_REFAULT | PAGE_FLAG_COW)));
    assert(pte & PAGE_FLAG_RW);
    assert(*(unsigned char *)(pte & PAGE_MASK) == 0xa5);
    assert(((pte & PAGE_MASK) == (uint64_t)original) == (owners == 1));
    // Even after read-only mprotect, private contents must be inherited.
    struct vma v = file_vma(0x400000, 4096);
    assert(!fork_can_refault(pte & ~PAGE_FLAG_RW, &v, v.start));
  }
}
int main(void) {
  compressed_fork(); all_clean(); mixed_and_oom(0);
  for (unsigned fail = 1; fail <= 6; fail++) mixed_and_oom(fail);
  exclusions(); cow_provenance(); reset();
  for (unsigned i = 0; i < used; i++) free(frames[i].ptr);
}
'''
with tempfile.TemporaryDirectory(prefix='avory-lazy-fork-') as tmp:
    path = Path(tmp)
    (path / 'test.c').write_text(prefix + source + tests)
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                    '-I', str(root), str(path / 'test.c'), '-o', str(path / 'test')],
                   check=True)
    subprocess.run([str(path / 'test')], check=True)
print('PASS: lazy fork, private COW provenance, inherited mappings, and OOM unwind')
