#include "string.h"

typedef uint64_t __attribute__((__may_alias__, aligned(1))) u64_ua;
typedef uint32_t __attribute__((__may_alias__, aligned(1))) u32_ua;
typedef uint16_t __attribute__((__may_alias__, aligned(1))) u16_ua;

#define ONES64  0x0101010101010101ULL
#define HIGHS64 0x8080808080808080ULL

static inline uint64_t has_zero_byte(uint64_t v) {
  return (v - ONES64) & ~v & HIGHS64;
}

int strcmp(const char *s1, const char *s2) {
  // Check bytes until s1 is 8-byte aligned or difference/null is reached
  while (((uintptr_t)s1 & 7) && *s1 && (*s1 == *s2)) {
    s1++;
    s2++;
  }
  if (!*s1 || *s1 != *s2) {
    return *(const unsigned char *)s1 - *(const unsigned char *)s2;
  }

  while (1) {
    // If s2 is near the end of a 4KB page, avoid reading past the page boundary
    if (__builtin_expect((((uintptr_t)s2 & 4095) > 4096 - 8), 0)) {
      for (int i = 0; i < 8; i++) {
        if (!*s1 || *s1 != *s2)
          return *(const unsigned char *)s1 - *(const unsigned char *)s2;
        s1++;
        s2++;
      }
      continue;
    }

    uint64_t v1 = *(const uint64_t *)s1;
    uint64_t v2 = *(const u64_ua *)s2;

    if (v1 != v2) {
      for (int i = 0; i < 8; i++) {
        unsigned char c1 = (unsigned char)s1[i];
        unsigned char c2 = (unsigned char)s2[i];
        if (c1 != c2 || c1 == 0)
          return c1 - c2;
      }
    }

    if (has_zero_byte(v1)) {
      return 0; // Matched all the way to null terminator
    }

    s1 += 8;
    s2 += 8;
  }
}

int strncmp(const char *s1, const char *s2, size_t n) {
  while (n && ((uintptr_t)s1 & 7) && *s1 && (*s1 == *s2)) {
    s1++;
    s2++;
    n--;
  }
  if (n == 0)
    return 0;
  if (!*s1 || *s1 != *s2) {
    return *(const unsigned char *)s1 - *(const unsigned char *)s2;
  }

  while (n >= 8) {
    if (__builtin_expect((((uintptr_t)s2 & 4095) > 4096 - 8), 0)) {
      for (int i = 0; i < 8; i++) {
        if (!*s1 || *s1 != *s2)
          return *(const unsigned char *)s1 - *(const unsigned char *)s2;
        s1++;
        s2++;
        n--;
      }
      continue;
    }

    uint64_t v1 = *(const uint64_t *)s1;
    uint64_t v2 = *(const u64_ua *)s2;

    if (v1 != v2) {
      for (int i = 0; i < 8; i++) {
        unsigned char c1 = (unsigned char)s1[i];
        unsigned char c2 = (unsigned char)s2[i];
        if (c1 != c2 || c1 == 0)
          return c1 - c2;
      }
    }

    if (has_zero_byte(v1)) {
      return 0;
    }

    s1 += 8;
    s2 += 8;
    n -= 8;
  }

  while (n && *s1 && (*s1 == *s2)) {
    s1++;
    s2++;
    n--;
  }
  if (n == 0)
    return 0;
  return *(const unsigned char *)s1 - *(const unsigned char *)s2;
}

size_t strlen(const char *s) {
  const char *orig = s;

  // Align s to 8-byte boundary
  while ((uintptr_t)s & 7) {
    if (!*s)
      return (size_t)(s - orig);
    s++;
  }

  const uint64_t *w = (const uint64_t *)s;
  while (1) {
    uint64_t v0 = w[0];
    uint64_t m0 = has_zero_byte(v0);
    if (m0)
      return (size_t)((const char *)w - orig) + (__builtin_ctzll(m0) >> 3);

    uint64_t v1 = w[1];
    uint64_t m1 = has_zero_byte(v1);
    if (m1)
      return (size_t)((const char *)(w + 1) - orig) + (__builtin_ctzll(m1) >> 3);

    w += 2;
  }
}

char *strcpy(char *dest, const char *src) {
  char *orig_dest = dest;

  // Align src to 8-byte boundary
  while ((uintptr_t)src & 7) {
    if (!(*dest++ = *src++))
      return orig_dest;
  }

  while (1) {
    uint64_t v = *(const uint64_t *)src;
    uint64_t z = has_zero_byte(v);
    if (z) {
      int len = __builtin_ctzll(z) >> 3;
      for (int i = 0; i <= len; i++) {
        dest[i] = src[i];
      }
      return orig_dest;
    }
    *(u64_ua *)dest = v;
    src += 8;
    dest += 8;
  }
}

char *strncpy(char *dest, const char *src, size_t n) {
  char *orig_dest = dest;

  while (n && ((uintptr_t)src & 7)) {
    if (!(*dest++ = *src++)) {
      n--;
      while (n--)
        *dest++ = '\0';
      return orig_dest;
    }
    n--;
  }

  while (n >= 8) {
    uint64_t v = *(const uint64_t *)src;
    uint64_t z = has_zero_byte(v);
    if (z) {
      int len = __builtin_ctzll(z) >> 3;
      for (int i = 0; i <= len; i++) {
        dest[i] = src[i];
      }
      dest += (len + 1);
      n -= (len + 1);
      while (n--)
        *dest++ = '\0';
      return orig_dest;
    }
    *(u64_ua *)dest = v;
    src += 8;
    dest += 8;
    n -= 8;
  }

  while (n) {
    if (!(*dest++ = *src++)) {
      n--;
      while (n--)
        *dest++ = '\0';
      return orig_dest;
    }
    n--;
  }

  return orig_dest;
}

char *strchr(const char *s, int c) {
  uint8_t ch = (uint8_t)c;
  if (ch == 0)
    return (char *)s + strlen(s);

  while ((uintptr_t)s & 7) {
    if (*s == (char)ch)
      return (char *)s;
    if (!*s)
      return NULL;
    s++;
  }

  uint64_t c8 = ch * ONES64;
  const uint64_t *w = (const uint64_t *)s;

  while (1) {
    uint64_t v = *w;
    uint64_t z = has_zero_byte(v);
    uint64_t match = v ^ c8;
    uint64_t m = has_zero_byte(match);

    if (z || m) {
      const char *p = (const char *)w;
      for (int i = 0; i < 8; i++) {
        if ((uint8_t)p[i] == ch)
          return (char *)p + i;
        if (p[i] == 0)
          return NULL;
      }
    }
    w++;
  }
}

char *strnchrnul(const char *s, size_t count, int c) {
  while (count-- && *s && *s != (char)c)
    s++;
  return (char *)s;
}

char *strrchr(const char *s, int c) {
  char *last = 0;
  do {
    if (*s == (char)c)
      last = (char *)s;
  } while (*s++);
  return last;
}

char *strcat(char *dest, const char *src) {
  char *ptr = dest;
  while (*ptr)
    ptr++;
  while ((*ptr++ = *src++))
    ;
  return dest;
}

char *strncat(char *dest, const char *src, size_t n) {
  char *ptr = dest;
  while (*ptr)
    ptr++;
  while (n && (*ptr++ = *src++))
    n--;
  if (n == 0)
    *ptr = '\0';
  return dest;
}

char *strstr(const char *haystack, const char *needle) {
  if (!*needle)
    return (char *)haystack;
  for (; *haystack; haystack++) {
    if (*haystack == *needle) {
      const char *h = haystack;
      const char *n = needle;
      while (*h && *n && *h == *n) {
        h++;
        n++;
      }
      if (!*n)
        return (char *)haystack;
    }
  }
  return NULL;
}


void *memset(void *s, int c, size_t n) {
  void *d = s;
  uint8_t *p = (uint8_t *)s;
  uint64_t c8 = (uint8_t)c * 0x0101010101010101ULL;

  if (n < 16) {
    if (n >= 8) {
      *(u64_ua *)p = c8;
      *(u64_ua *)(p + n - 8) = c8;
      return d;
    }
    if (n >= 4) {
      *(u32_ua *)p = (uint32_t)c8;
      *(u32_ua *)(p + n - 4) = (uint32_t)c8;
      return d;
    }
    if (n >= 2) {
      *(u16_ua *)p = (uint16_t)c8;
      *(u16_ua *)(p + n - 2) = (uint16_t)c8;
      return d;
    }
    if (n == 1)
      *p = (uint8_t)c;
    return d;
  }

  if (n <= 32) {
    *(u64_ua *)p = c8;
    *(u64_ua *)(p + 8) = c8;
    *(u64_ua *)(p + n - 16) = c8;
    *(u64_ua *)(p + n - 8) = c8;
    return d;
  }

  if (n <= 64) {
    *(u64_ua *)p = c8;
    *(u64_ua *)(p + 8) = c8;
    *(u64_ua *)(p + 16) = c8;
    *(u64_ua *)(p + 24) = c8;
    *(u64_ua *)(p + n - 32) = c8;
    *(u64_ua *)(p + n - 24) = c8;
    *(u64_ua *)(p + n - 16) = c8;
    *(u64_ua *)(p + n - 8) = c8;
    return d;
  }

  /* For large blocks, ERMS rep stosb provides hardware-accelerated fills */
  if (n >= 2048) {
    size_t cnt = n;
    __asm__ volatile("rep stosb"
                     : "+D"(p), "+c"(cnt)
                     : "a"((uint8_t)c)
                     : "memory");
    return d;
  }

  /* 64 to 2048 bytes: unrolled 64-byte quadword pipeline */
  while (n >= 64) {
    *(u64_ua *)(p)      = c8;
    *(u64_ua *)(p + 8)  = c8;
    *(u64_ua *)(p + 16) = c8;
    *(u64_ua *)(p + 24) = c8;
    *(u64_ua *)(p + 32) = c8;
    *(u64_ua *)(p + 40) = c8;
    *(u64_ua *)(p + 48) = c8;
    *(u64_ua *)(p + 56) = c8;
    p += 64;
    n -= 64;
  }

  while (n >= 8) {
    *(u64_ua *)p = c8;
    p += 8;
    n -= 8;
  }
  if (n >= 4) {
    *(u32_ua *)p = (uint32_t)c8;
    p += 4;
    n -= 4;
  }
  if (n >= 2) {
    *(u16_ua *)p = (uint16_t)c8;
    p += 2;
    n -= 2;
  }
  if (n == 1) {
    *p = (uint8_t)c;
  }

  return d;
}

void *memcpy(void *dest, const void *src, size_t n) {
  void *d = dest;
  uint8_t *dst = (uint8_t *)dest;
  const uint8_t *s = (const uint8_t *)src;

  if (n < 16) {
    if (n >= 8) {
      *(u64_ua *)dst = *(const u64_ua *)s;
      *(u64_ua *)(dst + n - 8) = *(const u64_ua *)(s + n - 8);
      return d;
    }
    if (n >= 4) {
      *(u32_ua *)dst = *(const u32_ua *)s;
      *(u32_ua *)(dst + n - 4) = *(const u32_ua *)(s + n - 4);
      return d;
    }
    if (n >= 2) {
      *(u16_ua *)dst = *(const u16_ua *)s;
      *(u16_ua *)(dst + n - 2) = *(const u16_ua *)(s + n - 2);
      return d;
    }
    if (n == 1)
      *dst = *s;
    return d;
  }

  if (n <= 32) {
    *(u64_ua *)dst = *(const u64_ua *)s;
    *(u64_ua *)(dst + 8) = *(const u64_ua *)(s + 8);
    *(u64_ua *)(dst + n - 16) = *(const u64_ua *)(s + n - 16);
    *(u64_ua *)(dst + n - 8) = *(const u64_ua *)(s + n - 8);
    return d;
  }

  if (n <= 64) {
    *(u64_ua *)dst = *(const u64_ua *)s;
    *(u64_ua *)(dst + 8) = *(const u64_ua *)(s + 8);
    *(u64_ua *)(dst + 16) = *(const u64_ua *)(s + 16);
    *(u64_ua *)(dst + 24) = *(const u64_ua *)(s + 24);
    *(u64_ua *)(dst + n - 32) = *(const u64_ua *)(s + n - 32);
    *(u64_ua *)(dst + n - 24) = *(const u64_ua *)(s + n - 24);
    *(u64_ua *)(dst + n - 16) = *(const u64_ua *)(s + n - 16);
    *(u64_ua *)(dst + n - 8) = *(const u64_ua *)(s + n - 8);
    return d;
  }

  /* Multi-kilobyte transfers: ERMS rep movsb provides hardware-accelerated moves */
  if (n >= 2048) {
    size_t cnt = n;
    __asm__ volatile("rep movsb"
                     : "+D"(dst), "+S"(s), "+c"(cnt)
                     :
                     : "memory");
    return d;
  }

  /* 64 to 2048 bytes: Unrolled 64-byte quadword pipeline (ideal for IPC messages, audio, and Wayland) */
  while (n >= 64) {
    u64_ua q0 = *(const u64_ua *)(s);
    u64_ua q1 = *(const u64_ua *)(s + 8);
    u64_ua q2 = *(const u64_ua *)(s + 16);
    u64_ua q3 = *(const u64_ua *)(s + 24);
    u64_ua q4 = *(const u64_ua *)(s + 32);
    u64_ua q5 = *(const u64_ua *)(s + 40);
    u64_ua q6 = *(const u64_ua *)(s + 48);
    u64_ua q7 = *(const u64_ua *)(s + 56);

    *(u64_ua *)(dst)      = q0;
    *(u64_ua *)(dst + 8)  = q1;
    *(u64_ua *)(dst + 16) = q2;
    *(u64_ua *)(dst + 24) = q3;
    *(u64_ua *)(dst + 32) = q4;
    *(u64_ua *)(dst + 40) = q5;
    *(u64_ua *)(dst + 48) = q6;
    *(u64_ua *)(dst + 56) = q7;

    s += 64;
    dst += 64;
    n -= 64;
  }

  while (n >= 8) {
    *(u64_ua *)dst = *(const u64_ua *)s;
    dst += 8;
    s += 8;
    n -= 8;
  }
  if (n >= 4) {
    *(u32_ua *)dst = *(const u32_ua *)s;
    dst += 4;
    s += 4;
    n -= 4;
  }
  if (n >= 2) {
    *(u16_ua *)dst = *(const u16_ua *)s;
    dst += 2;
    s += 2;
    n -= 2;
  }
  if (n == 1) {
    *dst = *s;
  }

  return d;
}

void *memmove(void *dest, const void *src, size_t n) {
  void *d = dest;
  if (!n || dest == src)
    return dest;
  if ((uintptr_t)dest < (uintptr_t)src ||
      (uintptr_t)dest - (uintptr_t)src >= n) {
    return memcpy(dest, src, n);
  } else {
    /* Overlapping copy towards higher addresses: copy from the tail.
     *
     * This was `std; rep movsb; cld`, which leaks the direction flag. Those
     * are three separate instructions, so an interrupt or exception that
     * arrives after `std` and before `cld` runs its whole handler with
     * RFLAGS.DF set - and every memset/memcpy in that handler (both are REP
     * strings here) then walks *backwards*, writing over whatever sits below
     * its destination. A 8 MiB console_clear() inside a panic handler turning
     * into a descending zero-fill through read-only .rodata is exactly what
     * hung the machine after an unrelated fault. A plain descending loop keeps
     * RFLAGS alone.
     *
     * When the overlap offset (dest - src) is at least 8 bytes, descending
     * 32-byte / 8-byte unaligned stores are completely safe and 8x faster
     * than a byte loop (e.g. console scroll and ring buffer compaction). */
    const uint8_t *s = (const uint8_t *)src + n;
    uint8_t *dst = (uint8_t *)dest + n;
    if ((uintptr_t)dest - (uintptr_t)src >= 8) {
      while (n >= 32) {
        uint64_t w0 = *(const u64_ua *)(s - 8);
        uint64_t w1 = *(const u64_ua *)(s - 16);
        uint64_t w2 = *(const u64_ua *)(s - 24);
        uint64_t w3 = *(const u64_ua *)(s - 32);
        *(u64_ua *)(dst - 8) = w0;
        *(u64_ua *)(dst - 16) = w1;
        *(u64_ua *)(dst - 24) = w2;
        *(u64_ua *)(dst - 32) = w3;
        s -= 32;
        dst -= 32;
        n -= 32;
      }
      while (n >= 8) {
        s -= 8;
        dst -= 8;
        *(u64_ua *)dst = *(const u64_ua *)s;
        n -= 8;
      }
    }
    while (n--) {
      *--dst = *--s;
    }
  }
  return d;
}

static int memcpy_wc_use_rep(void) {
  /* Give QEMU (both KVM and TCG) and hardware with ERMS fast string operations
   * instead of a guest loop with MOVNTI per machine word. In KVM and TCG,
   * MOVNTI is an un-accelerated MMIO exit per 8 bytes, causing severe console lag.
   * REP MOVSQ is handled in kernel/host fastpath. */
  static uint8_t mode;
  uint8_t cached = __atomic_load_n(&mode, __ATOMIC_RELAXED);
  if (cached)
    return cached == 2;

  uint32_t eax = 1, ebx, ecx, edx;
  __asm__ volatile("cpuid"
                   : "+a"(eax), "=b"(ebx), "+c"(ecx), "=d"(edx));
  int use_rep = 0;
  if (ecx & (1U << 31)) {
    /* Running inside a hypervisor (KVM, QEMU TCG, Xen, VMware, Hyper-V).
     * Emulated MMIO/WC stores are vastly faster with REP MOVSQ than MOVNTI. */
    use_rep = 1;
  } else {
    /* On bare metal, check for ERMS (Enhanced REP MOVSB/STOSB) */
    uint32_t eax7 = 7, ecx7 = 0;
    uint32_t ebx7 = 0, edx7 = 0;
    __asm__ volatile("cpuid"
                     : "+a"(eax7), "=b"(ebx7), "+c"(ecx7), "+d"(edx7));
    if (ebx7 & (1U << 9)) {
      use_rep = 1;
    }
  }
  __atomic_store_n(&mode, use_rep ? 2 : 1, __ATOMIC_RELAXED);
  return use_rep;
}

void *memcpy_to_wc(void *dest, const void *src, size_t n) {
  uint8_t *d = dest;
  const uint8_t *s = src;

  /* Both MOVNTI and REP MOVSQ benefit from an aligned WC destination. */
  while (n && ((uintptr_t)d & 7)) {
    *d++ = *s++;
    n--;
  }

  if (memcpy_wc_use_rep()) {
    size_t qwords = n >> 3;
    __asm__ volatile("rep movsq"
                     : "+D"(d), "+S"(s), "+c"(qwords)
                     :
                     : "memory");
    n &= 7;
    while (n--)
      *d++ = *s++;
    return dest;
  }

  while (n >= 64) {
    __asm__ volatile(
        "movq 0(%[src]), %%rax\n\t"
        "movq 8(%[src]), %%rcx\n\t"
        "movq 16(%[src]), %%rdx\n\t"
        "movq 24(%[src]), %%r8\n\t"
        "movq 32(%[src]), %%r9\n\t"
        "movq 40(%[src]), %%r10\n\t"
        "movq 48(%[src]), %%r11\n\t"
        "movq 56(%[src]), %%r12\n\t"
        "movnti %%rax, 0(%[dst])\n\t"
        "movnti %%rcx, 8(%[dst])\n\t"
        "movnti %%rdx, 16(%[dst])\n\t"
        "movnti %%r8, 24(%[dst])\n\t"
        "movnti %%r9, 32(%[dst])\n\t"
        "movnti %%r10, 40(%[dst])\n\t"
        "movnti %%r11, 48(%[dst])\n\t"
        "movnti %%r12, 56(%[dst])"
        :
        : [dst] "r"(d), [src] "r"(s)
        : "rax", "rcx", "rdx", "r8", "r9", "r10", "r11", "r12",
          "memory");
    d += 64;
    s += 64;
    n -= 64;
  }

  while (n >= 8) {
    __asm__ volatile(
        "movq (%[src]), %%rax\n\t"
        "movnti %%rax, (%[dst])"
        :
        : [dst] "r"(d), [src] "r"(s)
        : "rax", "memory");
    d += 8;
    s += 8;
    n -= 8;
  }

  while (n--)
    *d++ = *s++;

  return dest;
}

int memcmp(const void *s1, const void *s2, size_t n) {
  const unsigned char *p1 = s1, *p2 = s2;
  while (n >= 8) {
    uint64_t a = *(const u64_ua *)p1;
    uint64_t b = *(const u64_ua *)p2;
    if (a != b)
      break;
    p1 += 8;
    p2 += 8;
    n -= 8;
  }
  while (n--) {
    if (*p1 != *p2) {
      return *p1 - *p2;
    }
    p1++;
    p2++;
  }
  return 0;
}

uint32_t atoui(const char *s) {
  uint32_t res = 0;
  while (*s >= '0' && *s <= '9') {
    res = res * 10 + (*s - '0');
    s++;
  }
  return res;
}

static char tolower_char(char c) {
  if (c >= 'A' && c <= 'Z')
    return c + ('a' - 'A');
  return c;
}

int strcasecmp(const char *s1, const char *s2) {
  while (*s1 && *s2) {
    char c1 = tolower_char(*s1);
    char c2 = tolower_char(*s2);
    if (c1 != c2)
      return (unsigned char)c1 - (unsigned char)c2;
    s1++;
    s2++;
  }
  return (unsigned char)*s1 - (unsigned char)*s2;
}

int strncasecmp(const char *s1, const char *s2, size_t n) {
  while (n && *s1 && *s2) {
    char c1 = tolower_char(*s1);
    char c2 = tolower_char(*s2);
    if (c1 != c2)
      return (unsigned char)c1 - (unsigned char)c2;
    s1++;
    s2++;
    n--;
  }
  if (n == 0)
    return 0;
  return (unsigned char)*s1 - (unsigned char)*s2;
}


/* Emit one character and advance the output position.  The argument is
 * ALWAYS evaluated, even when the buffer is full: call sites pass
 * side-effecting expressions (e.g. `*fmt++` in the literal loop), and
 * skipping the evaluation used to leave `fmt` parked on the same character
 * forever once the buffer filled up (found via amdgpu's 27-char workqueue
 * name formatted into alloc_workqueue()'s 24-byte WQ_NAME_LEN buffer). */
#define EMIT(c)                          \
    do {                                 \
        char emit_c_ = (char)(c);        \
        if (pos + 1 < size)              \
            buf[pos] = emit_c_;          \
        pos++;                           \
    } while (0)

/* Write `str` of `len` characters with padding into the buffer. */
static size_t emit_padded(char *buf, size_t size, size_t pos,
                          const char *str, size_t len,
                          int width, int left_align, char pad_char) {
    int w = (int)len;
    /* Pad on the left */
    if (!left_align && width > w) {
        for (int i = 0; i < width - w; i++)
            EMIT(pad_char);
    }
    /* Write the string */
    for (size_t i = 0; i < len; i++)
        EMIT(str[i]);
    /* Pad on the right */
    if (left_align && width > w) {
        for (int i = 0; i < width - w; i++)
            EMIT(' ');
    }
    return pos;
}

static int u64_to_buf(uint64_t val, unsigned int base, int uppercase,
                      char *tmp) {
    static const char lower[] = "0123456789abcdef";
    static const char upper[] = "0123456789ABCDEF";
    const char *digits = uppercase ? upper : lower;

    if (val == 0) {
        tmp[0] = '0';
        return 1;
    }

    char rev[66];
    int n = 0;
    while (val) {
        rev[n++] = digits[val % base];
        val /= base;
    }
    /* Reverse into tmp */
    for (int i = 0; i < n; i++)
        tmp[i] = rev[n - 1 - i];
    return n;
}

/* Symbolic errno names for %pe (subset of upstream errname()). */
static const char *string_errno_name(long err) {
    switch (err) {
    case 1:   return "EPERM";
    case 2:   return "ENOENT";
    case 3:   return "ESRCH";
    case 4:   return "EINTR";
    case 5:   return "EIO";
    case 6:   return "ENXIO";
    case 7:   return "E2BIG";
    case 8:   return "ENOEXEC";
    case 9:   return "EBADF";
    case 10:  return "ECHILD";
    case 11:  return "EAGAIN";
    case 12:  return "ENOMEM";
    case 13:  return "EACCES";
    case 14:  return "EFAULT";
    case 16:  return "EBUSY";
    case 17:  return "EEXIST";
    case 18:  return "EXDEV";
    case 19:  return "ENODEV";
    case 20:  return "ENOTDIR";
    case 21:  return "EISDIR";
    case 22:  return "EINVAL";
    case 23:  return "ENFILE";
    case 24:  return "EMFILE";
    case 25:  return "ENOTTY";
    case 27:  return "EFBIG";
    case 28:  return "ENOSPC";
    case 29:  return "ESPIPE";
    case 30:  return "EROFS";
    case 31:  return "EMLINK";
    case 32:  return "EPIPE";
    case 36:  return "ENAMETOOLONG";
    case 38:  return "ENOSYS";
    case 39:  return "ENOTEMPTY";
    case 40:  return "ELOOP";
    case 42:  return "ENOMSG";
    case 43:  return "EIDRM";
    case 61:  return "ENODATA";
    case 62:  return "ETIME";
    case 71:  return "EPROTO";
    case 74:  return "EBADMSG";
    case 75:  return "EOVERFLOW";
    case 84:  return "EILSEQ";
    case 88:  return "ENOTSOCK";
    case 89:  return "EDESTADDRREQ";
    case 90:  return "EMSGSIZE";
    case 92:  return "ENOPROTOOPT";
    case 93:  return "EPROTONOSUPPORT";
    case 95:  return "EOPNOTSUPP";
    case 97:  return "EAFNOSUPPORT";
    case 98:  return "EADDRINUSE";
    case 99:  return "EADDRNOTAVAIL";
    case 100: return "ENETDOWN";
    case 101: return "ENETUNREACH";
    case 103: return "ECONNABORTED";
    case 104: return "ECONNRESET";
    case 105: return "ENOBUFS";
    case 106: return "EISCONN";
    case 107: return "ENOTCONN";
    case 110: return "ETIMEDOUT";
    case 111: return "ECONNREFUSED";
    case 113: return "EHOSTUNREACH";
    case 114: return "EALREADY";
    case 115: return "EINPROGRESS";
    case 116: return "ESTALE";
    case 122: return "EDQUOT";
    case 125: return "ECANCELED";
    default:  return NULL;
    }
}

int vsnprintf(char *buf, size_t size, const char *fmt, va_list ap) {
    size_t pos = 0;

    /* Need at least 1 byte for the NUL terminator */
    if (!buf || size == 0)
        return 0;

    while (*fmt) {
        if (*fmt != '%') {
            EMIT(*fmt++);
            continue;
        }
        fmt++; /* skip '%' */

        /* ---- Parse flags ---- */
        int left_align = 0;
        int show_sign  = 0; /* '+' */
        int space_sign = 0; /* ' ' */
        int alt_form   = 0; /* '#' */
        char pad_char  = ' ';

        while (*fmt == '-' || *fmt == '0' || *fmt == '+' || *fmt == ' ' ||
               *fmt == '#') {
            if (*fmt == '-') left_align = 1;
            if (*fmt == '0' && !left_align) pad_char = '0';
            if (*fmt == '+') show_sign = 1;
            if (*fmt == ' ') space_sign = 1;
            if (*fmt == '#') alt_form = 1;
            fmt++;
        }
        /* Left-align overrides zero-pad */
        if (left_align) pad_char = ' ';

        /* ---- Parse width ---- */
        int width = 0;
        if (*fmt == '*') {
            width = va_arg(ap, int);
            fmt++;
            if (width < 0) {
                left_align = 1;
                width = -width;
            }
        } else {
            while (*fmt >= '0' && *fmt <= '9')
                width = width * 10 + (*fmt++ - '0');
        }

        /* ---- Parse precision ---- */
        int prec = -1; /* -1 means "not specified" */
        if (*fmt == '.') {
            fmt++;
            if (*fmt == '*') {
                /* "%.*s" takes the precision from an argument (stock
                 * drm_printf_indent() relies on it).  A negative value is
                 * treated as "not specified", as in C. */
                prec = va_arg(ap, int);
                fmt++;
                if (prec < 0)
                    prec = -1;
            } else {
                prec = 0;
                while (*fmt >= '0' && *fmt <= '9')
                    prec = prec * 10 + (*fmt++ - '0');
            }
        }

        /* ---- Parse length modifier ---- */
        int is_ll = 0, is_l = 0, is_z = 0;
        if (*fmt == 'l') {
            fmt++;
            if (*fmt == 'l') { is_ll = 1; fmt++; }
            else              { is_l  = 1; }
        } else if (*fmt == 'z') {
            is_z = 1; fmt++;
        }

        char conv = *fmt++;

        /* ---- Handle each conversion ---- */
        switch (conv) {
        case '%':
            EMIT('%');
            break;

        case 'c':
            {
                char ch = (char)va_arg(ap, int);
                char tmp[1] = { ch };
                pos = emit_padded(buf, size, pos, tmp, 1,
                                  width, left_align, ' ');
            }
            break;

        case 's':
            {
                const char *s = va_arg(ap, const char *);
                if (!s) s = "(null)";
                size_t slen = strlen(s);
                if (prec >= 0 && (size_t)prec < slen)
                    slen = (size_t)prec;
                pos = emit_padded(buf, size, pos, s, slen,
                                  width, left_align, ' ');
            }
            break;

        case 'd':
        case 'i':
            {
                int64_t val;
                if      (is_ll) val = (int64_t)va_arg(ap, long long);
                else if (is_l)  val = (int64_t)va_arg(ap, long);
                else if (is_z)  val = (int64_t)va_arg(ap, int64_t);
                else            val = (int64_t)va_arg(ap, int);

                char tmp[66];
                int  neg = 0;
                int  n;
                if (val < 0) { neg = 1; val = -val; }
                n = u64_to_buf((uint64_t)val, 10, 0, tmp);

                /* Apply precision: minimum digit count */
                int digits = n;
                if (prec > digits) digits = prec;
                char sign = neg ? '-' : (show_sign ? '+' : (space_sign ? ' ' : 0));
                int sign_len = sign ? 1 : 0;
                int total = digits + sign_len;

                if (!left_align && pad_char == '0') {
                    /* sign then zeros then digits */
                    if (sign) EMIT(sign);
                    for (int i = total - sign_len; i < width - sign_len; i++)
                        EMIT('0');
                    for (int i = digits - n; i > 0; i--) EMIT('0');
                    for (int i = 0; i < n; i++) EMIT(tmp[i]);
                } else {
                    /* build into a local array for emit_padded */
                    char full[80];
                    int  fi = 0;
                    if (sign) full[fi++] = sign;
                    for (int i = digits - n; i > 0; i--) full[fi++] = '0';
                    for (int i = 0; i < n; i++) full[fi++] = tmp[i];
                    pos = emit_padded(buf, size, pos, full, (size_t)fi,
                                      width, left_align, pad_char);
                }
            }
            break;

        case 'u':
        case 'x':
        case 'X':
        case 'o':
            {
                uint64_t val;
                if      (is_ll) val = (uint64_t)va_arg(ap, unsigned long long);
                else if (is_l)  val = (uint64_t)va_arg(ap, unsigned long);
                else if (is_z)  val = (uint64_t)va_arg(ap, size_t);
                else            val = (uint64_t)va_arg(ap, unsigned int);

                unsigned int base = (conv == 'x' || conv == 'X') ? 16
                                  : (conv == 'o')                 ?  8
                                                                   : 10;
                int uppercase = (conv == 'X');

                char tmp[66];
                int n = u64_to_buf(val, base, uppercase, tmp);

                int digits = n;
                if (prec > digits) digits = prec;

                /* '%#x'/'%#X' prefix "0x"; '%#o' ensures a leading zero. */
                char prefix[2] = { 0, 0 };
                int prefix_len = 0;
                if (alt_form && val != 0) {
                    if (base == 16) {
                        prefix[0] = '0';
                        prefix[1] = uppercase ? 'X' : 'x';
                        prefix_len = 2;
                    } else if (base == 8 && tmp[0] != '0') {
                        prefix[0] = '0';
                        prefix_len = 1;
                    }
                }

                if (!left_align && pad_char == '0') {
                    for (int i = 0; i < prefix_len; i++) EMIT(prefix[i]);
                    for (int i = digits + prefix_len; i < width; i++) EMIT('0');
                    for (int i = digits - n; i > 0; i--) EMIT('0');
                    for (int i = 0; i < n; i++) EMIT(tmp[i]);
                } else {
                    char full[80];
                    int  fi = 0;
                    for (int i = 0; i < prefix_len; i++) full[fi++] = prefix[i];
                    for (int i = digits - n; i > 0; i--) full[fi++] = '0';
                    for (int i = 0; i < n; i++) full[fi++] = tmp[i];
                    pos = emit_padded(buf, size, pos, full, (size_t)fi,
                                      width, left_align, pad_char);
                }
            }
            break;

        case 'p':
            {
                char ext = *fmt;
                uintptr_t val;

                /* Kernel pointer extensions (mirrors lib/vsprintf.c). */
                if (ext == 'V') {
                    /* %pV: struct va_format { const char *fmt; va_list *va; } */
                    struct {
                        const char *fmt;
                        va_list *va;
                    } *vaf = va_arg(ap, void *);
                    fmt++;
                    if (vaf && vaf->fmt && vaf->va) {
                        char vbuf[256];
                        int vn = vsnprintf(vbuf, sizeof(vbuf), vaf->fmt,
                                           *vaf->va);
                        if (vn > 0)
                            pos = emit_padded(buf, size, pos, vbuf, (size_t)vn,
                                              width, left_align, ' ');
                    }
                    break;
                }

                if (ext == 'h') {
                    /* %*ph / %*phN / %phN: space-separated hex bytes. */
                    static const char hexd[] = "0123456789abcdef";
                    const unsigned char *data;
                    char hbuf[160];
                    int hn = 0;
                    int count = width;

                    fmt++;
                    if (*fmt >= '0' && *fmt <= '9') {
                        count = 0;
                        while (*fmt >= '0' && *fmt <= '9')
                            count = count * 10 + (*fmt++ - '0');
                    }
                    data = va_arg(ap, const unsigned char *);
                    if (count < 0)
                        count = 0;
                    for (int i = 0; i < count && hn + 3 < (int)sizeof(hbuf);
                         i++) {
                        if (i)
                            hbuf[hn++] = ' ';
                        hbuf[hn++] = hexd[data[i] >> 4];
                        hbuf[hn++] = hexd[data[i] & 0xf];
                    }
                    pos = emit_padded(buf, size, pos, hbuf, (size_t)hn, width,
                                      left_align, ' ');
                    break;
                }

                if (ext == 'M') {
                    /* %pM: colon-separated MAC address. */
                    static const char hexd[] = "0123456789abcdef";
                    const unsigned char *m = va_arg(ap, const unsigned char *);
                    char mbuf[18];
                    int mn = 0;

                    fmt++;
                    for (int i = 0; i < 6; i++) {
                        if (i)
                            mbuf[mn++] = ':';
                        mbuf[mn++] = hexd[m[i] >> 4];
                        mbuf[mn++] = hexd[m[i] & 0xf];
                    }
                    pos = emit_padded(buf, size, pos, mbuf, (size_t)mn, width,
                                      left_align, ' ');
                    break;
                }

                if (ext == 'a') {
                    /* %pa: physical address (always full-width hex). */
                    const uint64_t *pa = va_arg(ap, const uint64_t *);
                    char tmp[66];
                    char full[80];
                    int fi = 0;
                    int n = u64_to_buf(pa ? *pa : 0, 16, 0, tmp);

                    fmt++;
                    full[fi++] = '0';
                    full[fi++] = 'x';
                    for (int i = 0; i < 16 - n; i++)
                        full[fi++] = '0';
                    for (int i = 0; i < n; i++)
                        full[fi++] = tmp[i];
                    pos = emit_padded(buf, size, pos, full, (size_t)fi, width,
                                      left_align, ' ');
                    break;
                }

                if (ext == 'e' || ext == 'E') {
                    /* %pe: ERR_PTR as its symbolic errno, else signed decimal
                     * (same shape as upstream err_ptr()). */
                    long err = (long)va_arg(ap, void *);
                    const char *name = string_errno_name(err < 0 ? -err : err);
                    char ebuf[32];
                    int en = 0;

                    fmt++;
                    if (name) {
                        while (*name && en < (int)sizeof(ebuf) - 1)
                            ebuf[en++] = *name++;
                    } else {
                        char num[24];
                        int nn = u64_to_buf(
                            (uint64_t)(err < 0 ? -err : err), 10, 0, num);
                        if (err < 0)
                            ebuf[en++] = '-';
                        for (int i = 0; i < nn && en < (int)sizeof(ebuf) - 1;
                             i++)
                            ebuf[en++] = num[i];
                    }
                    pos = emit_padded(buf, size, pos, ebuf, (size_t)en, width,
                                      left_align, ' ');
                    break;
                }

                if (ext == 'S' || ext == 'B' || ext == 'F' || ext == 's' ||
                    ext == 'K' || ext == 'x') {
                    /* Symbolic printing needs kallsyms; the address is the
                     * best available representation. */
                    fmt++;
                }

                val = (uintptr_t)va_arg(ap, void *);
                {
                    char tmp[66];
                    int n = u64_to_buf((uint64_t)val, 16, 0, tmp);
                    /* "0x" prefix + zero-padded to pointer width */
                    int ptr_digits = (int)(sizeof(void *) * 2);
                    int digits = n > ptr_digits ? n : ptr_digits;
                    int total  = digits + 2; /* "0x" */

                    char full[80];
                    int fi = 0;
                    full[fi++] = '0';
                    full[fi++] = 'x';
                    for (int i = digits - n; i > 0; i--) full[fi++] = '0';
                    for (int i = 0; i < n; i++) full[fi++] = tmp[i];
                    pos = emit_padded(buf, size, pos, full, (size_t)(total > fi ? fi : total),
                                      width, left_align, ' ');
                }
            }
            break;

        default:
            /* Unknown specifier — emit literally */
            EMIT('%');
            EMIT(conv);
            break;
        }
    }

    /* NUL-terminate; pos is the logical length (may be >= size) */
    buf[pos < size ? pos : size - 1] = '\0';
    return (int)pos;
}

#undef EMIT

int snprintf(char *buf, size_t size, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int ret = vsnprintf(buf, size, fmt, ap);
    va_end(ap);
    return ret;
}

int vscnprintf(char *buf, size_t size, const char *fmt, va_list args) {
    int n = vsnprintf(buf, size, fmt, args);
    return n < (int)size ? n : (int)size - 1;
}

int scnprintf(char *buf, size_t size, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int ret = vscnprintf(buf, size, fmt, ap);
    va_end(ap);
    return ret;
}

int sprintf(char *buf, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    /* Unbounded write — caller must ensure buf is large enough. */
    int ret = vsnprintf(buf, (size_t)-1, fmt, ap);
    va_end(ap);
    return ret;
}
