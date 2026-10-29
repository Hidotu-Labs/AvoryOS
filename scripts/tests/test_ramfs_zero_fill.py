#!/usr/bin/env python3
"""RAMFS extension and memfd mmap must not expose recycled bytes."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]


def function(path, signature):
    source = (ROOT / path).read_text()
    start = source.index(signature)
    end = source.index('{', start) + 1
    depth = 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end] + '\n'


code = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PAGE_SIZE 4096
#define FS_FILE 1
#define FS_PIPE 3
#define FS_TYPE_MASK 0xff
#define MAP_SHARED 1
#define MAP_PRIVATE 2
#define PAGE_FLAG_PRESENT (1ULL << 0)
#define PAGE_FLAG_RW (1ULL << 1)
#define PAGE_FLAG_USER (1ULL << 2)
#define PAGE_FLAG_NX (1ULL << 63)
#define PAGE_FLAG_COW (1ULL << 9)

typedef struct {
  uint8_t *data;
  uint32_t capacity;
  uint8_t data_is_pmm;
} ramfs_file_t;

typedef struct vfs_node {
  void *device;
  uint32_t length;
  uint32_t flags;
} vfs_node_t;

struct thread { int unused; };

static struct thread current_thread;
static uint64_t page_maps;

static void *pmm_alloc_pages(size_t pages) {
  size_t bytes = pages * PAGE_SIZE;
  void *p = malloc(bytes);
  if (p) memset(p, 0xa5, bytes); /* mimic recycled, dirty PMM frames */
  return p;
}
static uint64_t pmm_get_hhdm_offset(void) { return 0; }
static void pmm_free_pages(void *p, size_t pages) { (void)pages; free(p); }
static bool pmm_is_managed(uint64_t phys) { (void)phys; return false; }
static void pmm_incref(void *p) { (void)p; }
static void *kmalloc(size_t bytes) {
  void *p = malloc(bytes);
  if (p) memset(p, 0xa5, bytes);
  return p;
}
static void kfree(void *p) { free(p); }
static bool is_user_ptr(uint64_t p) { (void)p; return false; }
static unsigned long copy_from_user(void *dst, const void *src, size_t n) {
  memcpy(dst, src, n); return 0;
}
static void ramfs_free_file_data(ramfs_file_t *file) {
  if (file->data) {
    if (file->data_is_pmm)
      pmm_free_pages(file->data, file->capacity / PAGE_SIZE);
    else
      kfree(file->data);
  }
  file->data = NULL; file->capacity = 0; file->data_is_pmm = 0;
}
static struct thread *sched_get_current(void) { return &current_thread; }
static uint64_t *vmm_get_active_pml4(void) { static uint64_t pml4[512]; return pml4; }
static uint64_t mm_alloc_mmap_region(uint64_t len) { (void)len; return 0x400000; }
static uint64_t vmm_virt_to_phys(uint64_t *pml4, uint64_t va) {
  (void)pml4; (void)va; return 0;
}
static bool vmm_map_page(uint64_t *pml4, uint64_t va, uint64_t phys,
                         uint64_t flags) {
  (void)pml4; (void)va; (void)phys; (void)flags; page_maps++; return true;
}
'''

code += function('kernel/src/fs/ramfs.c', 'static uint8_t *ramfs_alloc_buffer(')
code += function('kernel/src/fs/ramfs.c', 'uint32_t ramfs_write(')
code += function('kernel/src/syscalls/sys_aio.c', 'static uint64_t memfd_mmap(')

code += r'''
int main(void) {
  uint8_t value = 0x7e;
  ramfs_file_t file = {.data_is_pmm = 1};
  vfs_node_t node = {.device = &file, .flags = FS_FILE};

  /* Wine grows an anonymous mapping by writing one byte at its final offset.
     Every preceding byte must remain zero in the MAP_SHARED memfd view. */
  assert(ramfs_write(&node, 65536, 1, &value) == 1);
  assert(node.length == 65537 && file.capacity >= node.length);
  for (uint32_t i = 0; i < 65536; i++) assert(file.data[i] == 0);
  assert(file.data[65536] == value);
  for (uint32_t i = node.length; i < file.capacity; i++) assert(file.data[i] == 0);

  /* A write after truncation must not reveal the old contents in the hole. */
  memset(file.data + 32, 0xc3, 64);
  node.length = 32;
  assert(ramfs_write(&node, 64, 1, &value) == 1);
  for (uint32_t i = 32; i < 64; i++) assert(file.data[i] == 0);
  assert(file.data[64] == value);

  /* mmap can extend EOF while capacity already exists; zero stale truncated
     bytes before publishing them as part of the mapping. */
  memset(file.data + 128, 0xd4, file.capacity - 128);
  node.length = 128;
  assert(memfd_mmap(&node, 0x400000, PAGE_SIZE, 3, MAP_SHARED, 0) == 0x400000);
  assert(node.length == PAGE_SIZE && page_maps == 1);
  for (uint32_t i = 128; i < PAGE_SIZE; i++) assert(file.data[i] == 0);

  ramfs_free_file_data(&file);

  /* The small kmalloc-backed RAMFS path must also be zero-filled. */
  ramfs_file_t small = {0};
  vfs_node_t small_node = {.device = &small, .flags = FS_FILE};
  assert(ramfs_write(&small_node, 80, 1, &value) == 1);
  for (uint32_t i = 0; i < 80; i++) assert(small.data[i] == 0);
  assert(small.data[80] == value);
  ramfs_free_file_data(&small);

  puts("PASS: RAMFS writes and memfd mappings zero recycled bytes and file holes");
  return 0;
}
'''

with tempfile.TemporaryDirectory(prefix='avory-ramfs-zero-fill-') as tmp:
    cfile = Path(tmp) / 'test.c'
    binary = Path(tmp) / 'test'
    cfile.write_text(code)
    subprocess.run(['cc', '-Wall', '-Wextra', '-Werror', '-fsanitize=undefined',
                    str(cfile), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
