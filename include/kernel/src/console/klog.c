#include "klog.h"
#include "../drivers/serial.h"
#include "../fb/framebuffer.h"
#include "../font/font.h"
#include "../lib/string.h"
#include "../lock/spinlock.h"
#include "console.h"
#include <stdarg.h>

static spinlock_t klog_lock = SPINLOCK_INIT;
static bool screen_logging_enabled = false;
static uint32_t screen_x = 0;
static uint32_t screen_y = 0;

#define KLOG_FG 0x00FFFFFF
#define KLOG_BG 0x00000000

static uint32_t klog_fg = KLOG_FG;

static bool esc = false;
static int esc_state = 0;
static char esc_buffer[32];
static int esc_idx = 0;

static uint32_t klog_ansi_colors[8] = {
    0x00000000, // Black
    0x00FF0000, // Red
    0x0000FF00, // Green
    0x00FFFF00, // Yellow
    0x000000FF, // Blue
    0x00FF00FF, // Magenta
    0x0000FFFF, // Cyan
    0x00FFFFFF  // White
};

void klog_set_screen_logging(bool enabled) {
  spinlock_acquire(&klog_lock);
  screen_logging_enabled = enabled;
  spinlock_release(&klog_lock);
}

static void klog_putchar_screen_unlocked(char c) {
  if (!screen_logging_enabled)
    return;

  if (esc) {
    if (esc_state == 0) {
      if (c == '[') {
        esc_state = 1;
      } else {
        esc = false;
      }
    } else if (esc_state == 1) {
      if ((c >= '0' && c <= '9') || c == ';') {
        if (esc_idx < 31)
          esc_buffer[esc_idx++] = c;
      } else if (c == 'm') {
        esc_buffer[esc_idx] = '\0';
        int code = 0;
        for (int i = 0; esc_buffer[i]; i++) {
          if (esc_buffer[i] == ';') {
            code = 0;
            continue;
          }
          code = code * 10 + (esc_buffer[i] - '0');
        }

        if (code == 0) {
          klog_fg = KLOG_FG;
        } else if (code >= 30 && code <= 37) {
          klog_fg = klog_ansi_colors[code - 30];
        } else if (code >= 90 && code <= 97) {
          klog_fg = klog_ansi_colors[code - 90];
        }

        esc = false;
      } else {
        esc = false;
      }
    }
    return;
  }

  if (c == '\x1b') {
    esc = true;
    esc_state = 0;
    esc_idx = 0;
    return;
  }

  uint32_t w = fb_get_width();
  uint32_t h = fb_get_height();

  if (c == '\n') {
    screen_x = 0;
    screen_y += FONT_HEIGHT;
  } else if (c == '\r') {
    screen_x = 0;
  } else {
    if (screen_x + FONT_WIDTH > w) {
      screen_x = 0;
      screen_y += FONT_HEIGHT;
    }

    if (screen_y + FONT_HEIGHT > h) {
      screen_y = 0;
    }

    const uint8_t *glyph = font_get_glyph(c);
    for (uint32_t gy = 0; gy < FONT_HEIGHT; gy++) {
      fb_draw_glyph_scanline(screen_x, screen_y + gy, glyph[gy], klog_fg,
                             KLOG_BG);
    }
    screen_x += FONT_WIDTH;
  }
}

static inline void klog_write_dispatch(const char *s, size_t len) {
  if (!s || len == 0)
    return;

  serial_write(s, len);

  if (__builtin_expect(screen_logging_enabled, 0)) {
    /* trylock: if this CPU already holds klog_lock (a fault inside the screen
     * path) or another CPU is mid-draw, skip the framebuffer instead of
     * deadlocking the exception path. */
    if (spinlock_try_acquire(&klog_lock)) {
      for (size_t i = 0; i < len; i++) {
        klog_putchar_screen_unlocked(s[i]);
      }
      spinlock_release(&klog_lock);
    }
  }
}

void klog_putchar(char c) {
  serial_putchar(c);
  if (__builtin_expect(screen_logging_enabled, 0)) {
    if (spinlock_try_acquire(&klog_lock)) {
      klog_putchar_screen_unlocked(c);
      spinlock_release(&klog_lock);
    }
  }
}

void klog_puts(const char *s) {
  if (!s)
    return;

  size_t len = 0;
  while (s[len])
    len++;

  klog_write_dispatch(s, len);
}

void klog_uint64(uint64_t num) {
  char buf[24];
  if (num == 0) {
    buf[0] = '0';
    buf[1] = '\0';
    klog_write_dispatch(buf, 1);
    return;
  }

  char tmp[24];
  int i = 0;
  while (num > 0) {
    tmp[i++] = '0' + (num % 10);
    num /= 10;
  }
  int len = 0;
  while (i > 0) {
    buf[len++] = tmp[--i];
  }
  buf[len] = '\0';

  klog_write_dispatch(buf, (size_t)len);
}

void klog_int64(int64_t num) {
  if (num < 0) {
    klog_putchar('-');
    klog_uint64((uint64_t)(-num));
  } else {
    klog_uint64((uint64_t)num);
  }
}

void klog_hex64(uint64_t num) {
  char buf[20];
  const char *hex = "0123456789ABCDEF";
  buf[0] = '0';
  buf[1] = 'x';
  for (int i = 0; i < 16; i++) {
    buf[2 + i] = hex[(num >> (60 - i * 4)) & 0xF];
  }
  buf[18] = '\0';

  klog_write_dispatch(buf, 18);
}

void klog_hex32(uint32_t num) {
  char buf[12];
  const char *hex = "0123456789ABCDEF";
  buf[0] = '0';
  buf[1] = 'x';
  for (int i = 0; i < 8; i++) {
    buf[2 + i] = hex[(num >> (28 - i * 4)) & 0xF];
  }
  buf[10] = '\0';

  klog_write_dispatch(buf, 10);
}

void vklogf(const char *fmt, va_list ap) {
  if (!fmt)
    return;

  char buf[512];
  int len = vsnprintf(buf, sizeof(buf), fmt, ap);
  if (len <= 0)
    return;

  if ((size_t)len >= sizeof(buf))
    len = (int)sizeof(buf) - 1;

  klog_write_dispatch(buf, (size_t)len);
}

void klogf(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vklogf(fmt, ap);
  va_end(ap);
}

void klog_proc_exit(uint32_t tid, uint32_t tgid, bool is_thread, const char *comm, uint64_t status) {
  char buf[160];
  char *p = buf;

  const char pfx[] = "[PROC] exit tid=";
  for (size_t i = 0; i < sizeof(pfx) - 1; i++)
    *p++ = pfx[i];

  char num_buf[24];
  int ni = 0;
  uint64_t n = tid;
  if (n == 0) {
    num_buf[ni++] = '0';
  } else {
    while (n > 0) {
      num_buf[ni++] = '0' + (n % 10);
      n /= 10;
    }
  }
  while (ni > 0)
    *p++ = num_buf[--ni];

  const char tgid_s[] = " tgid=";
  for (size_t i = 0; i < sizeof(tgid_s) - 1; i++)
    *p++ = tgid_s[i];
  n = tgid;
  if (n == 0) {
    num_buf[ni++] = '0';
  } else {
    while (n > 0) {
      num_buf[ni++] = '0' + (n % 10);
      n /= 10;
    }
  }
  while (ni > 0)
    *p++ = num_buf[--ni];

  const char *kind_str = is_thread ? " kind=thread comm=" : " kind=process comm=";
  while (*kind_str)
    *p++ = *kind_str++;

  if (comm && *comm) {
    while (*comm)
      *p++ = *comm++;
  } else {
    *p++ = '?';
  }

  const char st_s[] = " status=";
  for (size_t i = 0; i < sizeof(st_s) - 1; i++)
    *p++ = st_s[i];
  n = status;
  if (n == 0) {
    num_buf[ni++] = '0';
  } else {
    while (n > 0) {
      num_buf[ni++] = '0' + (n % 10);
      n /= 10;
    }
  }
  while (ni > 0)
    *p++ = num_buf[--ni];

  *p++ = '\n';

  klog_write_dispatch(buf, (size_t)(p - buf));

  if (status != 0) {
    char warn[160];
    int wlen = snprintf(warn, sizeof(warn), "[PROC] WARNING: tid=%u comm=%s exited with failure status=%llu\n",
                        tid, comm ? comm : "?", (unsigned long long)status);
    if (wlen > 0)
      klog_write_dispatch(warn, (size_t)wlen);
  }
}

void klog_proc_exec(uint32_t tid, const char *path) {
  char buf[256];
  char *p = buf;

  const char pfx[] = "[PROC] exec tid=";
  for (size_t i = 0; i < sizeof(pfx) - 1; i++)
    *p++ = pfx[i];

  char num_buf[24];
  int ni = 0;
  uint64_t n = tid;
  if (n == 0) {
    num_buf[ni++] = '0';
  } else {
    while (n > 0) {
      num_buf[ni++] = '0' + (n % 10);
      n /= 10;
    }
  }
  while (ni > 0)
    *p++ = num_buf[--ni];

  const char path_s[] = " path=";
  for (size_t i = 0; i < sizeof(path_s) - 1; i++)
    *p++ = path_s[i];

  if (path && *path) {
    while (*path && (size_t)(p - buf) < sizeof(buf) - 2)
      *p++ = *path++;
  } else {
    *p++ = '?';
  }

  *p++ = '\n';

  klog_write_dispatch(buf, (size_t)(p - buf));

  if (path && (strstr(path, "badwolf") || strstr(path, "bwrap") || strstr(path, "WebKit") || strstr(path, "webkit"))) {
    char note[160];
    int nlen = snprintf(note, sizeof(note), "[PROC] [WEBKIT/BROWSER] Launching %s (tid=%u)\n",
                        path, tid);
    if (nlen > 0)
      klog_write_dispatch(note, (size_t)nlen);
  }
}

void klog_ramfs_free(void *ptr, uint64_t capacity, bool is_pmm, uint64_t pages) {
#if KLOG_VERBOSE
  char buf[160];
  char *p = buf;

  if (is_pmm) {
    const char pfx[] = "[RAMFS] free PMM-backed data ptr=0x";
    for (size_t i = 0; i < sizeof(pfx) - 1; i++)
      *p++ = pfx[i];
  } else {
    const char pfx[] = "[RAMFS] free heap-backed data ptr=0x";
    for (size_t i = 0; i < sizeof(pfx) - 1; i++)
      *p++ = pfx[i];
  }

  const char *hex = "0123456789abcdef";
  uint64_t uptr = (uint64_t)(uintptr_t)ptr;
  for (int i = 60; i >= 0; i -= 4) {
    *p++ = hex[(uptr >> i) & 0xF];
  }

  const char cap_s[] = " capacity=";
  for (size_t i = 0; i < sizeof(cap_s) - 1; i++)
    *p++ = cap_s[i];
  char num_buf[24];
  int ni = 0;
  uint64_t n = capacity;
  if (n == 0) {
    num_buf[ni++] = '0';
  } else {
    while (n > 0) {
      num_buf[ni++] = '0' + (n % 10);
      n /= 10;
    }
  }
  while (ni > 0)
    *p++ = num_buf[--ni];

  if (is_pmm) {
    const char pgs_s[] = " pages=";
    for (size_t i = 0; i < sizeof(pgs_s) - 1; i++)
      *p++ = pgs_s[i];
    ni = 0;
    n = pages;
    if (n == 0) {
      num_buf[ni++] = '0';
    } else {
      while (n > 0) {
        num_buf[ni++] = '0' + (n % 10);
        n /= 10;
      }
    }
    while (ni > 0)
      *p++ = num_buf[--ni];
  }

  *p++ = '\n';

  klog_write_dispatch(buf, (size_t)(p - buf));
#else
  (void)ptr;
  (void)capacity;
  (void)is_pmm;
  (void)pages;
#endif
}
