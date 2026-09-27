// sys_random.c — getrandom syscall
#include "../apic/lapic_timer.h"
#include "../console/klog.h"
#include "hal/hal.h"
#include "../sched/sched.h"
#include "sys_io_shared.h"
#include "syscall.h"
#include <stdint.h>

static uint64_t prng_state = 0;

static void prng_seed(void) {
  if (prng_state != 0)
    return;
  uint64_t tsc = hal_cpu_cycle_count();
  uint64_t ticks = lapic_timer_get_ticks();
  prng_state = tsc ^ (ticks << 32) ^ 0xDEADBEEFCAFEBABEULL;
  if (prng_state == 0)
    prng_state = 1;
}

static uint64_t xorshift64(void) {
  uint64_t x = prng_state;
  x ^= x << 13;
  x ^= x >> 7;
  x ^= x << 17;
  prng_state = x;
  return x;
}

static uint64_t sys_getrandom(uint64_t buf_ptr, uint64_t buflen, uint64_t flags,
                              uint64_t a3, uint64_t a4, uint64_t a5) {
  (void)flags;
  (void)a3;
  (void)a4;
  (void)a5;
  if (!buf_ptr || !is_user_ptr(buf_ptr))
    return (uint64_t)-14;
  if (buflen == 0)
    return 0;

  prng_seed();
  uint8_t *buf = (uint8_t *)buf_ptr;
  uint64_t written = 0;

  while (written < buflen) {
    uint64_t rand_val = xorshift64();
    for (int i = 0; i < 8 && written < buflen; i++) {
      buf[written++] = (uint8_t)(rand_val & 0xFF);
      rand_val >>= 8;
    }
  }
  return written;
}

void syscall_register_random(void) {
  syscall_register(SYS_GETRANDOM, sys_getrandom);
}
