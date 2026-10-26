#define _POSIX_C_SOURCE 200809L
#include "mm/zram.h"
#include "mm/zram/lz4.h"
#include <assert.h>
#include <lz4.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static atomic_int fail_heap, fail_page;
static atomic_long heap_live, page_live;
void *kmalloc(size_t n) {
  if (atomic_exchange(&fail_heap, 0))
    return NULL;
  void *p = malloc(n);
  if (p)
    atomic_fetch_add(&heap_live, 1);
  return p;
}
void kfree(void *p) {
  if (p) {
    atomic_fetch_sub(&heap_live, 1);
    free(p);
  }
}
void *pmm_alloc(void) {
  if (atomic_exchange(&fail_page, 0))
    return NULL;
  void *p = NULL;
  if (posix_memalign(&p, 4096, 4096))
    return NULL;
  atomic_fetch_add(&page_live, 1);
  return p;
}
void pmm_free_page(void *p) {
  if (p) {
    atomic_fetch_sub(&page_live, 1);
    free(p);
  }
}
uint64_t pmm_get_hhdm_offset(void) { return 0; }
static uint64_t random64(uint64_t *s) {
  *s ^= *s << 13;
  *s ^= *s >> 7;
  *s ^= *s << 17;
  return *s;
}
static void fill(uint8_t *p, unsigned kind, uint64_t seed) {
  uint64_t r = seed | 1;
  for (unsigned i = 0; i < 4096; i++) {
    if (kind == 0)
      p[i] = (uint8_t)seed;
    else if (kind == 1)
      p[i] = (i % 127 < 16) ? (uint8_t)random64(&r) : (uint8_t)(i / 127 + seed);
    else if (kind == 2)
      p[i] = (uint8_t)(i * 7 + seed);
    else
      p[i] = (uint8_t)random64(&r);
  }
}
static void codec(void) {
  uint8_t in[4096], encoded[8192], other[8192], out[4096], reference[4096];
  uint64_t r = 987654321;
  for (unsigned i = 0; i < 60000; i++) {
    fill(in, i % 4, random64(&r));
    size_t n = zram_lz4_compress(in, sizeof(in), encoded, sizeof(encoded));
    assert(n && n <= sizeof(encoded));
    assert(!zram_lz4_decompress(encoded, n, out, sizeof(out)) &&
           !memcmp(in, out, sizeof(in)));
    assert(LZ4_decompress_safe((char *)encoded, (char *)reference, (int)n,
                               4096) == 4096);
    assert(!memcmp(in, reference, sizeof(in)));
    int m =
        LZ4_compress_default((char *)in, (char *)other, 4096, sizeof(other));
    assert(m > 0);
    assert(!zram_lz4_decompress(other, (size_t)m, out, sizeof(out)) &&
           !memcmp(in, out, sizeof(in)));
    size_t cap = random64(&r) % 80;
    memset(encoded, 0x77, sizeof(encoded));
    size_t small = zram_lz4_compress(in, 4096, encoded, cap);
    assert(small <= cap);
    for (size_t j = cap; j < sizeof(encoded); j++)
      assert(encoded[j] == 0x77);
  }
  for (unsigned i = 0; i < 60000; i++) {
    size_t n = random64(&r) % sizeof(encoded);
    for (size_t j = 0; j < n; j++)
      encoded[j] = (uint8_t)random64(&r);
    (void)zram_lz4_decompress(encoded, n, out, random64(&r) % 4097);
  }
  puts("PASS: 60000 LZ4 roundtrips in both directions against liblz4; 60000 "
       "malformed-block fuzz cases and bounded outputs");
}
static uint64_t baseline;
static void empty(void) {
  struct zram_stats s;
  zram_store_stats(&s);
  assert(!s.stored_pages && !s.same_pages && !s.compressed_bytes &&
         s.memory_bytes == baseline);
  assert(!atomic_load(&page_live) && atomic_load(&heap_live) == 1);
}
static void *stress(void *arg) {
  uint64_t r = (uintptr_t)arg * 789123 + 1;
  uint8_t in[4096], out[4096];
  for (unsigned i = 0; i < 20000; i++) {
    fill(in, i % 3, random64(&r));
    uint32_t slot = zram_store_page(in);
    assert(slot);
    assert(zram_slot_get(slot));
    zram_slot_put(slot); /* release creator; pin alone must keep data alive */
    assert(!zram_slot_read(slot, out) && !memcmp(in, out, sizeof(in)));
    assert(zram_slot_get(slot));
    zram_slot_put(slot);
    zram_slot_put(slot);
  }
  return NULL;
}
static void store(void) {
  assert(!zram_store_init(0, 512 * 1024));
  assert(!zram_store_init(524289, 512 * 1024));
  assert(!zram_store_init(4096, 4096));
  atomic_store(&fail_heap, 1);
  assert(!zram_store_init(4096, 512 * 1024));
  assert(zram_store_init(4096, 512 * 1024));
  assert(!zram_store_init(4096, 512 * 1024));
  struct zram_stats s;
  zram_store_stats(&s);
  baseline = s.memory_bytes;
  uint8_t in[4096], out[4096];
  uint32_t ids[4096];
  fill(in, 1, 99);
  atomic_store(&fail_heap, 1);
  assert(!zram_store_page(in));
  empty();
  atomic_store(&fail_page, 1);
  assert(!zram_store_page(in));
  empty();
  fill(in, 3, 987);
  assert(!zram_store_page(in));
  empty();
  fill(in, 0, 0);
  for (unsigned i = 0; i < 4096; i++) {
    ids[i] = zram_store_page(in);
    assert(ids[i]);
  }
  assert(!zram_store_page(in));
  zram_store_stats(&s);
  assert(s.same_pages == 4096 && s.memory_bytes == baseline);
  for (unsigned i = 0; i < 4096; i++)
    zram_slot_put(ids[(i * 127) % 4096]);
  empty();
  unsigned n = 0;
  for (; n < 4096; n++) {
    fill(in, 1, n + 1000);
    ids[n] = zram_store_page(in);
    if (!ids[n])
      break;
  }
  assert(n && n < 4096);
  zram_store_stats(&s);
  assert(s.memory_bytes <= s.memory_limit && s.memory_bytes > baseline);
  for (unsigned i = 0; i < n; i++) {
    fill(in, 1, i + 1000);
    assert(!zram_slot_read(ids[i], out) && !memcmp(in, out, 4096));
    zram_slot_put(ids[i]);
  }
  empty();
  pthread_t threads[8];
  for (uintptr_t i = 0; i < 8; i++)
    assert(!pthread_create(&threads[i], NULL, stress, (void *)(i + 1)));
  for (unsigned i = 0; i < 8; i++)
    assert(!pthread_join(threads[i], NULL));
  empty();
  assert(!zram_slot_get(0) && !zram_slot_get(4097));
  assert(zram_slot_read(0, out) < 0 && zram_slot_read(4097, out) < 0);
  zram_slot_put(0);
  zram_slot_put(4097);
  empty();
  puts("PASS: allocation failure rollback, incompressible pages, full slot "
       "table, hard storage limit, slot reuse and complete packed-frame "
       "reclamation");
  puts("PASS: 8 threads x 20000 compressed-object lifetimes with independent "
       "reader pins and byte-for-byte checks");
}
int main(void) {
  codec();
  store();
  return 0;
}
