/* SPDX-License-Identifier: GPL-2.0-only */
/* Identifier bytes for imported lib/uuid.c only. This is NOT the Linux
 * cryptographic random API: hardware RNG is preferred, but the fallback
 * provides boot-local identifier diversity rather than secret material.
 * A per-object compiler rename keeps future get_random_bytes callers from
 * linking against this adapter accidentally. */
#include <linux/types.h>
#include <linux/string.h>
#include <linuxkpi/native.h>
#include <linuxkpi/native_sched.h>

static u64 guid_sequence;

static bool guid_hardware_word(u64 *word) {
  unsigned a, b, c, d;
  __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d)
                   : "0"(1), "2"(0));
  if (!(c & (1U << 30)))
    return false;
  for (int i = 0; i < 10; i++) {
    unsigned char ok;
    __asm__ volatile("rdrand %0; setc %1" : "=r"(*word), "=qm"(ok) : : "cc");
    if (ok)
      return true;
  }
  return false;
}

/* SplitMix64 finalizer: a bijection, so distinct sequence inputs stay distinct
 * before the UUID library reserves its version/variant bits. */
static u64 guid_mix(u64 word) {
  word = (word ^ (word >> 30)) * 0xbf58476d1ce4e5b9ULL;
  word = (word ^ (word >> 27)) * 0x94d049bb133111ebULL;
  return word ^ (word >> 31);
}

void linuxkpi_guid_bytes(void *buffer, size_t len) {
  unsigned char *out = buffer;
  while (len) {
    u64 word;
    if (!guid_hardware_word(&word)) {
      u64 sequence = __atomic_add_fetch(&guid_sequence, 1, __ATOMIC_RELAXED);
      word = guid_mix(sequence * 0x9e3779b97f4a7c15ULL ^
                      linuxkpi_rdtsc_fence() ^ linuxkpi_monotonic_ns());
    }
    size_t count = len < sizeof(word) ? len : sizeof(word);
    memcpy(out, &word, count);
    out += count;
    len -= count;
  }
}
