#!/usr/bin/env python3
"""A failed HTree insertion must leave index bytes out of the linear allocator."""
from pathlib import Path
import os
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
source = (ROOT / 'kernel/src/fs/ext2/ext2_dir.c').read_text()
start = source.index('int ext2_add_dir_entry(')
end = source.index('\nint ext2_remove_dir_entry(', start)
function = source[start:end]
harness = r'''
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#define EXT2_INDEX_FL 0x1000
#define EXT4_EXTENTS_FL 0x80000
#define BS 1024
struct inode { uint32_t i_flags, i_size, i_blocks; };
typedef struct inode ext2_inode_t;
typedef struct { uint32_t block_size; } ext2_mount_t;
typedef struct __attribute__((packed)) {
  uint32_t inode; uint16_t rec_len; uint8_t name_len, file_type; char name[];
} ext2_dirent_t;
static unsigned char disk[BS], original[BS];
static int indexed_result, writes, indexed_calls;
static int ext2_read_inode(ext2_mount_t *m, uint32_t n, ext2_inode_t *out) {
  (void)m; (void)n;
  *out = (ext2_inode_t){EXT2_INDEX_FL, BS, 2}; return 0;
}
static int ext4_dx_add_entry(ext2_mount_t *m, uint32_t n, ext2_inode_t *i,
                            uint32_t child, const char *name, uint8_t type) {
  (void)m; (void)n; (void)i; (void)child; (void)name; (void)type;
  indexed_calls++; return indexed_result;
}
#define kmalloc malloc
#define kfree free
static uint64_t ext2_get_block_num(ext2_mount_t *m, ext2_inode_t *i, uint32_t b) {
  (void)m; (void)i; (void)b; return 1;
}
static int ext2_read_block(ext2_mount_t *m, uint64_t b, void *out) {
  (void)m; (void)b; memcpy(out, disk, BS); return 0;
}
static int ext3_journal_block(ext2_mount_t *m, uint64_t b, const void *in) {
  (void)m; (void)b; writes++; memcpy(disk, in, BS); return 0;
}
static int ext4_alloc_extent(ext2_mount_t *m, ext2_inode_t *i, uint32_t n,
                            uint32_t b, int count, uint64_t *out) {
  (void)m; (void)i; (void)n; (void)b; (void)count; (void)out; abort();
}
static uint64_t ext2_alloc_block(ext2_mount_t *m) { (void)m; abort(); }
static int ext2_set_block_num(ext2_mount_t *m, ext2_inode_t *i, uint32_t b, uint64_t p) {
  (void)m; (void)i; (void)b; (void)p; abort();
}
static void ext2_free_block(ext2_mount_t *m, uint64_t b) { (void)m; (void)b; abort(); }
static int ext2_write_inode(ext2_mount_t *m, uint32_t n, ext2_inode_t *i) {
  (void)m; (void)n; (void)i; abort();
}
'''
harness += function
harness += r'''
int main(void) {
  ext2_mount_t mount = {BS};
  ext2_dirent_t *dot = (void *)disk, *parent = (void *)(disk + 12);
  dot->inode = 42; dot->rec_len = 12; dot->name_len = 1; dot->name[0] = '.';
  parent->inode = 2; parent->rec_len = BS - 12; parent->name_len = 2;
  memcpy(parent->name, "..", 2);
  /* Real HTree root info and count/limit occupy the '..' record's slack. */
  disk[29] = 8; disk[32] = 124; disk[34] = 1;
  memcpy(original, disk, BS);
  int results[] = {-1, -5, -12, 0};
  for (unsigned n = 0; n < sizeof(results)/sizeof(results[0]); n++) {
    indexed_result = results[n]; indexed_calls = writes = 0;
    int rc = ext2_add_dir_entry(&mount, 42, 77, "ld-linux.so.2", 7);
    assert(rc == indexed_result);
    assert(indexed_calls == 1);
    assert(writes == 0);
    assert(memcmp(disk, original, BS) == 0);
  }
}
'''
with tempfile.TemporaryDirectory(prefix='htree-insert-') as temp:
    path = Path(temp)
    (path / 'test.c').write_text(harness)
    subprocess.run(['cc', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
                    str(path / 'test.c'), '-o', str(path / 'test')], check=True)
    # LeakSanitizer cannot inspect processes under the sandbox's ptrace.
    env = dict(os.environ, ASAN_OPTIONS=os.environ.get('ASAN_OPTIONS', '') + ':detect_leaks=0')
    subprocess.run([str(path / 'test')], check=True, env=env)
print('PASS: actual ext2_add_dir_entry preserves HTree index and propagates insertion results')
