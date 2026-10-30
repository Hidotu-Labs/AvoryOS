#!/usr/bin/env python3
"""Exercise the real MBR scanner with full logical-sector transfers under ASan."""
from pathlib import Path
import os
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
source = (ROOT / 'kernel/src/drivers/storage/mbr.c').read_text()
source = '\n'.join(line for line in source.splitlines() if not line.startswith('#include'))
header = (ROOT / 'kernel/src/drivers/storage/mbr.h').read_text()
harness = r'''
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
struct block_device {
  uint32_t sector_size;
  int (*read_sectors)(struct block_device *, uint64_t, uint32_t, void *);
};
static unsigned reads, added, allocated;
static bool signature, fail_read, fail_alloc;
static void *kmalloc(size_t size) {
  if (fail_alloc) return NULL;
  void *p = malloc(size); if (p) allocated++; return p;
}
static void kfree(void *p) { if (p) allocated--; free(p); }
static int block_add_partition(struct block_device *dev, int index,
                                uint64_t start, uint64_t count) {
  (void)dev; assert(index == 1); assert(start == 2048); assert(count == 100);
  added++; return 0;
}
'''
harness += header + '\n' + source
harness += r'''
static int read_sector(struct block_device *dev, uint64_t lba, uint32_t n, void *buf) {
  assert(lba == 0 && n == 1); reads++;
  if (fail_read) return -1;
  /* Exactly what a 4 KiB-sector driver such as zram writes. ASan catches
   * the old 512-byte allocation even when its neighbours happen to be idle. */
  memset(buf, 0, dev->sector_size ? dev->sector_size : 512);
  struct mbr *m = buf;
  if (signature) {
    m->signature = MBR_SIGNATURE;
    m->partitions[0].type = 0x83;
    m->partitions[0].start_lba = 2048;
    m->partitions[0].total_sectors = 100;
  }
  return 0;
}
int main(void) {
  uint32_t sizes[] = {512, 4096, 0};
  for (unsigned i = 0; i < sizeof(sizes)/sizeof(sizes[0]); i++) {
    struct block_device dev = {sizes[i], read_sector};
    reads = added = 0; signature = true;
    assert(mbr_scan_partitions(&dev) == 1);
    assert(reads == 1 && added == 1 && allocated == 0);
    reads = added = 0; signature = false;
    assert(mbr_scan_partitions(&dev) == -1);
    assert(reads == 1 && added == 0 && allocated == 0);
    fail_read = true;
    assert(mbr_scan_partitions(&dev) == -1 && allocated == 0);
    fail_read = false; fail_alloc = true; reads = 0;
    assert(mbr_scan_partitions(&dev) == -1 && reads == 0);
    fail_alloc = false;
  }
  struct block_device tiny = {256, read_sector}; reads = 0;
  assert(mbr_scan_partitions(&tiny) == -1 && reads == 0);
  assert(mbr_scan_partitions(NULL) == -1);
}
'''
with tempfile.TemporaryDirectory(prefix='mbr-sector-') as temp:
    path = Path(temp)
    (path / 'test.c').write_text(harness)
    subprocess.run(['cc', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
                    str(path / 'test.c'), '-o', str(path / 'test')], check=True)
    env = dict(os.environ, ASAN_OPTIONS=os.environ.get('ASAN_OPTIONS', '') + ':detect_leaks=0')
    subprocess.run([str(path / 'test')], check=True, env=env)
print('PASS: native MBR scanner handles 512/4096-byte sectors, invalid geometry and error cleanup')
