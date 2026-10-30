#!/usr/bin/env python3
"""Compare actual range protection with the original per-page membership rule."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
source = (ROOT / 'kernel/src/mm/pmm_reclaim.c').read_text()
functions = source[source.index('bool pmm_is_reclaimable('):
                   source.index('void pmm_reclaim_bootloader(')]
harness = r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#define PAGE_SIZE 4096ULL
#define LIMINE_MEMMAP_BOOTLOADER_RECLAIMABLE 5
#define PAGES (4 * 1024 * 1024 * 1024ULL / PAGE_SIZE)
struct limine_memmap_entry { uint64_t base, length, type; };
struct map { uint64_t entry_count; struct limine_memmap_entry **entries; };
static struct map *pmm_internal_memmap;
static unsigned char actual[PAGES], expected[PAGES];
static void pmm_mark_used(void *ptr, size_t count) {
  assert(count == 1);
  uint64_t page = (uint64_t)ptr / PAGE_SIZE;
  assert(page < PAGES);
  actual[page] = 1;
}
'''
harness += functions
harness += r'''
static void check(uint64_t phys, size_t count) {
  memset(actual, 0, sizeof(actual));
  memset(expected, 0, sizeof(expected));
  for (size_t i = 0; i < count; i++) {
    uint64_t page = phys + i * PAGE_SIZE;
    if (pmm_is_reclaimable(page)) expected[page / PAGE_SIZE] = 1;
  }
  pmm_protect_reclaimable_range(phys, count);
  assert(!memcmp(actual, expected, sizeof(actual)));
}
int main(void) {
  struct limine_memmap_entry regions[] = {
    {0, PAGE_SIZE * 2, 5}, {PAGE_SIZE * 2, PAGE_SIZE * 10, 0},
    {0x200000 - PAGE_SIZE, PAGE_SIZE * 3, 5},
    {0x40000001, PAGE_SIZE * 3 + 11, 5},
    {0x50000000, PAGE_SIZE * 20, 0},
    {0x7ffff000, PAGE_SIZE * 5, 5},
    {0xc0000000, PAGE_SIZE * 100, 5},
  };
  struct limine_memmap_entry *entries[sizeof(regions)/sizeof(regions[0])];
  for (unsigned i=0; i<sizeof(entries)/sizeof(entries[0]); i++) entries[i]=&regions[i];
  struct map map={sizeof(entries)/sizeof(entries[0]), entries};
  pmm_internal_memmap=&map;
  check(0, 0); check(0, 512); check(0x200000, 512);
  for (uint64_t base=0; base<4*1024*1024*1024ULL; base+=1024*1024*1024ULL)
    check(base, 0x40000);
  check(0x40000000, 1); check(0x40001000, 4);
  map.entry_count=0; check(0, 512);
  pmm_internal_memmap=NULL; check(0, 512);
}
'''
with tempfile.TemporaryDirectory(prefix='boot-reclaim-') as directory:
    path = Path(directory)
    (path / 'test.c').write_text(harness)
    subprocess.run(['cc', '-O2', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=undefined', str(path / 'test.c'), '-o', str(path / 'test')], check=True)
    subprocess.run([str(path / 'test')], check=True)
print('PASS: range protection matches per-page checks across 2 MiB/1 GiB mappings, boundaries, holes and empty maps')
