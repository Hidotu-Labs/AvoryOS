// sys_random.c — getrandom syscall
#include "../lib/csprng.h"
#include "sys_io_shared.h"
#include "syscall.h"
#include <stdint.h>

static uint64_t sys_getrandom(uint64_t buf_ptr, uint64_t buflen, uint64_t flags,
                              uint64_t a3, uint64_t a4, uint64_t a5) {
  (void)flags;
  (void)a3;
  (void)a4;
  (void)a5;
  if (buflen == 0)
    return 0;
  if (!buf_ptr || !is_user_range((const void *)buf_ptr, buflen))
    return (uint64_t)-14;

  uint8_t *user_buf = (uint8_t *)buf_ptr;
  uint64_t written = 0;

  while (written < buflen) {
    uint8_t chunk[256];
    uint64_t remaining = buflen - written;
    size_t chunk_len = remaining < sizeof(chunk) ? (size_t)remaining : sizeof(chunk);
    csprng_get_bytes(chunk, chunk_len);
    if (copy_to_user(user_buf + written, chunk, chunk_len) != 0)
      return written ? written : (uint64_t)-14;
    written += chunk_len;
  }
  return written;
}

void syscall_register_random(void) {
  syscall_register(SYS_GETRANDOM, sys_getrandom);
}
