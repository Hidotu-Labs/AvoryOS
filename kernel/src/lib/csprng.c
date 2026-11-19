#include "csprng.h"
#include "string.h"
#include "../apic/lapic_timer.h"
#include "../drivers/timer/rtc.h"
#include "../lock/spinlock.h"
#include "hal/hal.h"

#define CSPRNG_RESEED_BYTES (1024U * 1024U)

static spinlock_t csprng_lock = SPINLOCK_INIT;
static uint32_t csprng_key[8];
static uint64_t csprng_counter = 0;
static uint64_t csprng_bytes_since_reseed = 0;
static bool csprng_seeded = false;

static inline uint32_t rotl32(uint32_t v, int n) {
  return (v << n) | (v >> (32 - n));
}

static inline void chacha_qr(uint32_t *a, uint32_t *b, uint32_t *c, uint32_t *d) {
  *a += *b; *d ^= *a; *d = rotl32(*d, 16);
  *c += *d; *b ^= *c; *b = rotl32(*b, 12);
  *a += *b; *d ^= *a; *d = rotl32(*d, 8);
  *c += *d; *b ^= *c; *b = rotl32(*b, 7);
}

static void chacha20_block(const uint32_t key[8], uint64_t ctr, uint8_t out[64]) {
  static const uint32_t sigma[4] = {0x61707865U, 0x3320646eU, 0x79622d32U, 0x6b206574U};
  uint32_t st[16];
  st[0] = sigma[0];
  st[1] = sigma[1];
  st[2] = sigma[2];
  st[3] = sigma[3];
  for (int i = 0; i < 8; i++)
    st[4 + i] = key[i];
  st[12] = (uint32_t)(ctr & 0xffffffffU);
  st[13] = (uint32_t)(ctr >> 32);
  st[14] = 0x524f5641U; /* "AVOR" */
  st[15] = 0x474e5259U; /* "YRNG" */

  uint32_t x[16];
  for (int i = 0; i < 16; i++)
    x[i] = st[i];

  for (int i = 0; i < 10; i++) {
    chacha_qr(&x[0], &x[4], &x[8], &x[12]);
    chacha_qr(&x[1], &x[5], &x[9], &x[13]);
    chacha_qr(&x[2], &x[6], &x[10], &x[14]);
    chacha_qr(&x[3], &x[7], &x[11], &x[15]);
    chacha_qr(&x[0], &x[5], &x[10], &x[15]);
    chacha_qr(&x[1], &x[6], &x[11], &x[12]);
    chacha_qr(&x[2], &x[7], &x[8], &x[13]);
    chacha_qr(&x[3], &x[4], &x[9], &x[14]);
  }

  for (int i = 0; i < 16; i++) {
    uint32_t w = x[i] + st[i];
    out[i * 4 + 0] = (uint8_t)(w & 0xffU);
    out[i * 4 + 1] = (uint8_t)((w >> 8) & 0xffU);
    out[i * 4 + 2] = (uint8_t)((w >> 16) & 0xffU);
    out[i * 4 + 3] = (uint8_t)((w >> 24) & 0xffU);
  }
}

static bool hw_rdseed64(uint64_t *out) {
  uint32_t eax = 0, ebx = 0, ecx = 0, edx = 0;
  __asm__ volatile("cpuid" : "+a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "c"(0));
  if (eax < 7)
    return false;
  eax = 7;
  ecx = 0;
  __asm__ volatile("cpuid" : "+a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx));
  if (!(ebx & (1U << 18)))
    return false;
  for (int i = 0; i < 10; i++) {
    uint64_t val = 0;
    uint8_t ok = 0;
    __asm__ volatile("rdseed %0; setc %1" : "=r"(val), "=qm"(ok));
    if (ok) {
      *out = val;
      return true;
    }
  }
  return false;
}

static bool hw_rdrand64(uint64_t *out) {
  uint32_t eax = 1, ebx = 0, ecx = 0, edx = 0;
  __asm__ volatile("cpuid" : "+a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx));
  if (!(ecx & (1U << 30)))
    return false;
  for (int i = 0; i < 10; i++) {
    uint64_t val = 0;
    uint8_t ok = 0;
    __asm__ volatile("rdrand %0; setc %1" : "=r"(val), "=qm"(ok));
    if (ok) {
      *out = val;
      return true;
    }
  }
  return false;
}

static uint64_t splitmix64(uint64_t x) {
  x += 0x9e3779b97f4a7c15ULL;
  x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
  x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
  return x ^ (x >> 31);
}

static void csprng_reseed_locked(void) {
  uint64_t ent[4];
  uint64_t tsc = hal_cpu_cycle_count();
  uint64_t ticks = lapic_timer_get_ticks();
  uint64_t rtc = rtc_get_boot_timestamp();

  for (int i = 0; i < 4; i++) {
    uint64_t hw = 0;
    if (!hw_rdseed64(&hw) && !hw_rdrand64(&hw)) {
      hw = splitmix64(hal_cpu_cycle_count() ^ ((uint64_t)i << 56));
    }
    ent[i] = hw ^ splitmix64(tsc ^ (ticks << 21) ^ (rtc << 37) ^ (uint64_t)i);
  }

  for (int i = 0; i < 8; i++) {
    uint32_t word = (uint32_t)(ent[i >> 1] >> ((i & 1) * 32));
    csprng_key[i] ^= word;
  }

  uint8_t block[64];
  chacha20_block(csprng_key, csprng_counter++, block);
  memcpy(csprng_key, block, 32);
  memset(block, 0, sizeof(block));
  memset(ent, 0, sizeof(ent));
  csprng_bytes_since_reseed = 0;
  csprng_seeded = true;
}

void csprng_stir(const void *data, size_t len) {
  if (!data || !len)
    return;
  hal_irq_state_t irq = hal_irq_save();
  spinlock_acquire(&csprng_lock);
  if (!csprng_seeded)
    csprng_reseed_locked();
  const uint8_t *bytes = (const uint8_t *)data;
  uint8_t key_bytes[32];
  memcpy(key_bytes, csprng_key, sizeof(key_bytes));
  for (size_t i = 0; i < len; i++) {
    key_bytes[i & 31U] ^= bytes[i];
    if ((i & 31U) == 31U || i + 1 == len) {
      uint8_t block[64];
      memcpy(csprng_key, key_bytes, sizeof(csprng_key));
      chacha20_block(csprng_key, csprng_counter++, block);
      memcpy(key_bytes, block, sizeof(key_bytes));
      memset(block, 0, sizeof(block));
    }
  }
  memcpy(csprng_key, key_bytes, sizeof(csprng_key));
  memset(key_bytes, 0, sizeof(key_bytes));
  spinlock_release(&csprng_lock);
  hal_irq_restore(irq);
}

void csprng_get_bytes(void *out, size_t len) {
  if (!out || !len)
    return;
  uint8_t *dst = (uint8_t *)out;
  size_t off = 0;
  uint8_t block[64];

  hal_irq_state_t irq = hal_irq_save();
  spinlock_acquire(&csprng_lock);

  if (!csprng_seeded || csprng_bytes_since_reseed >= CSPRNG_RESEED_BYTES)
    csprng_reseed_locked();

  while (off < len) {
    chacha20_block(csprng_key, csprng_counter++, block);
    size_t take = len - off;
    if (take > sizeof(block))
      take = sizeof(block);
    memcpy(dst + off, block, take);
    off += take;
    csprng_bytes_since_reseed += take;
  }

  /* Fast key erasure: replace current key so prior output cannot be derived. */
  chacha20_block(csprng_key, csprng_counter++, block);
  memcpy(csprng_key, block, sizeof(csprng_key));
  memset(block, 0, sizeof(block));

  spinlock_release(&csprng_lock);
  hal_irq_restore(irq);
}
