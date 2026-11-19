#include "klog.h"
#include "../drivers/serial.h"
#include "../fb/framebuffer.h"
#include "../font/font.h"
#include "../lib/string.h"
#include "../lock/spinlock.h"
#include "console.h"
#include "../smp/cpu.h"
#include <stdarg.h>

static spinlock_t klog_lock = SPINLOCK_INIT;
static bool screen_logging_enabled = false;
static bool console_sink_enabled = false;
static uint32_t screen_x = 0;
static uint32_t screen_y = 0;

/* Persistent boot log.  Serial capture is not always available (no cable,
 * fast boot, no reader attached), so the last KLOG_RING_SIZE bytes of every
 * kernel message are also kept in a ring buffer and exposed at /proc/klog.
 * The ring is written from every context (IRQs, faults, SMP bring-up), so it
 * has its own lock; a byte is dropped rather than deadlocking if the lock is
 * already held by the same CPU.
 *
 * Tracing builds can enlarge it (`make KLOG_RING_KB=4096`): browser syscall
 * traces produce megabytes, and the default 256 KiB wraps in well under a
 * second of WebKit activity. */
#ifndef KLOG_RING_KB
#define KLOG_RING_KB 256
#endif
#define KLOG_RING_SIZE (KLOG_RING_KB * 1024)
static char klog_ring[KLOG_RING_SIZE];
static uint32_t klog_ring_head;  /* next write index */
static uint32_t klog_ring_count; /* bytes currently stored */
static spinlock_t klog_ring_lock = SPINLOCK_INIT;

/* Boot-log snapshot.  The live ring keeps only the most recent bytes and
 * userland (process execs, services) fills it within seconds of starting, so
 * the driver bring-up lines would be gone by the time anyone can read
 * /proc/klog.  This copy stops at the first userspace exec and is served at
 * /proc/bootlog, which keeps the boot decisions readable indefinitely. */
#define KLOG_BOOT_SIZE (256 * 1024)
static char klog_boot[KLOG_BOOT_SIZE];
static uint32_t klog_boot_len;
static bool klog_boot_frozen;

/* Owner of klog_ring_lock (CPU index), or -1.  Another CPU holding the lock
 * is normal contention and we wait for it (the critical section is a memcpy);
 * only re-entry on the *same* CPU (a fault/NMI inside the append) drops the
 * record, since waiting there would deadlock. */
static volatile int32_t klog_ring_owner = -1;

static inline int32_t klog_this_cpu(void) {
  struct cpu_info *ci = cpu_get_current();
  return ci ? (int32_t)ci->cpu_id : 0;
}

static void klog_ring_copy_locked(const char *s, size_t len) {
  if (!klog_boot_frozen && klog_boot_len < KLOG_BOOT_SIZE) {
    size_t n = KLOG_BOOT_SIZE - klog_boot_len;
    if (n > len)
      n = len;
    memcpy(&klog_boot[klog_boot_len], s, n);
    klog_boot_len += (uint32_t)n;
  }
  if (len > KLOG_RING_SIZE) {
    s += len - KLOG_RING_SIZE;
    len = KLOG_RING_SIZE;
  }
  size_t first = KLOG_RING_SIZE - klog_ring_head;
  if (first > len)
    first = len;
  memcpy(&klog_ring[klog_ring_head], s, first);
  if (len > first)
    memcpy(&klog_ring[0], s + first, len - first);
  klog_ring_head = (uint32_t)((klog_ring_head + len) % KLOG_RING_SIZE);
  klog_ring_count += (uint32_t)len;
  if (klog_ring_count > KLOG_RING_SIZE)
    klog_ring_count = KLOG_RING_SIZE;
}

static void klog_ring_append(const char *s, size_t len) {
  int32_t me = klog_this_cpu();
  uint32_t spins = 0;
  while (!spinlock_try_acquire(&klog_ring_lock)) {
    if (__atomic_load_n(&klog_ring_owner, __ATOMIC_RELAXED) == me)
      return; /* same-CPU re-entry: drop rather than deadlock */
    if (++spins > 10000000u)
      return; /* wedged holder: never hang the logger */
    __asm__ volatile("pause" ::: "memory");
  }
  __atomic_store_n(&klog_ring_owner, me, __ATOMIC_RELAXED);
  klog_ring_copy_locked(s, len);
  __atomic_store_n(&klog_ring_owner, -1, __ATOMIC_RELAXED);
  spinlock_release(&klog_ring_lock);
}

void klog_freeze_boot_log(void) { klog_boot_frozen = true; }

uint32_t klog_bootlog_length(void) { return klog_boot_len; }

uint32_t klog_bootlog_read(uint32_t offset, uint32_t size, uint8_t *buffer) {
  if (offset >= klog_boot_len || !buffer)
    return 0;
  uint32_t n = klog_boot_len - offset;
  if (n > size)
    n = size;
  for (uint32_t i = 0; i < n; i++)
    buffer[i] = (uint8_t)klog_boot[offset + i];
  return n;
}

uint32_t klog_ring_length(void) { return klog_ring_count; }

uint32_t klog_ring_read(uint32_t offset, uint32_t size, uint8_t *buffer) {
  uint64_t flags;
  spinlock_acquire_save(&klog_ring_lock, &flags);
  if (offset >= klog_ring_count || !buffer) {
    spinlock_release_restore(&klog_ring_lock, flags);
    return 0;
  }
  uint32_t available = klog_ring_count - offset;
  if (size > available)
    size = available;
  uint32_t start = (klog_ring_head + KLOG_RING_SIZE - klog_ring_count +
                    offset) % KLOG_RING_SIZE;
  uint32_t first = KLOG_RING_SIZE - start;
  if (first > size)
    first = size;
  memcpy(buffer, &klog_ring[start], first);
  if (size > first)
    memcpy(buffer + first, &klog_ring[0], size - first);
  spinlock_release_restore(&klog_ring_lock, flags);
  return size;
}


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

void klog_set_console_sink(bool enabled) {
  spinlock_acquire(&klog_lock);
  console_sink_enabled = enabled;
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

static void klog_write_dispatch(const char *s, size_t len) {
  if (!s || len == 0)
    return;

  /* Keep a persistent copy for /proc/klog before anything can drop it. */
  klog_ring_append(s, len);

  /* Enqueue each record under one serial lock before drawing its screen copy.
   * The console's 128-byte mirror batches would otherwise split long reports
   * and allow another CPU's output to interrupt them. */
  /* Never use serial_write's direct-UART contention fallback for ordinary
   * records: it bypasses the queue and splices bytes into another CPU's log.
   * Queue the complete record; the BSP tick and idle loop drain it. */
  serial_enqueue(s, len);

  /* After console_init() the console owns the screen and keeps its own
   * cursor.  Route the screen copy through it so the boot log stays visible
   * on the framebuffer without emitting a second serial copy.
   * If the console is busy, fall back to serial-only rather than block: this
   * path runs from fault and hang diagnostics too. */
  if (__builtin_expect(screen_logging_enabled && console_sink_enabled, 0)) {
    if (console_write_screen_batch_try(s, len)) {
      /* A completed line is a debug checkpoint: push it to the front buffer
       * now so a hang on the next statement cannot leave the screen showing
       * an older frame that cuts the line in half. */
      if (s[len - 1] == '\n')
        console_tick();
      return;
    }
    return;
  }

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

/* Per-CPU line buffers.  Most call sites build one line from several
 * klog_puts/klog_uint64/klog_hex64 calls; sending each fragment straight to
 * the ring + serial costs a lock round trip apiece and lets other CPUs' output
 * splice into the middle of the line.  Fragments are collected here (IRQs off,
 * so nothing on this CPU interleaves) and dispatched as one record on '\n'
 * or when the buffer fills.  Re-entry on the same CPU (a fault or NMI while
 * the buffer is busy) bypasses the buffer and writes directly.
 *
 * vklogf formats into the same per-CPU storage, which keeps its 1.5 KiB
 * scratch buffer off the 16 KiB guard-less kernel stack. */
#define KLOG_LINE_SIZE 512
#define KLOG_FMT_SIZE 1536
struct klog_cpu_buf {
  char line[KLOG_LINE_SIZE];
  char fmt[KLOG_FMT_SIZE];
  uint32_t len;
  volatile uint32_t busy;
};
static struct klog_cpu_buf klog_pcpu[MAX_CPUS];

static inline struct klog_cpu_buf *klog_cpu_claim(hal_irq_state_t *flags) {
  *flags = hal_irq_save();
  int32_t cpu = klog_this_cpu();
  if (cpu < 0 || cpu >= MAX_CPUS) {
    hal_irq_restore(*flags);
    return NULL;
  }
  struct klog_cpu_buf *b = &klog_pcpu[cpu];
  if (b->busy) {
    hal_irq_restore(*flags);
    return NULL;
  }
  b->busy = 1;
  return b;
}

static inline void klog_cpu_unclaim(struct klog_cpu_buf *b,
                                    hal_irq_state_t flags) {
  b->busy = 0;
  hal_irq_restore(flags);
}

static void klog_line_flush_locked(struct klog_cpu_buf *b) {
  uint32_t n = b->len;
  if (n) {
    b->len = 0; /* before dispatch: panic-mode dispatch re-enters klog_flush */
    klog_write_dispatch(b->line, n);
  }
}

static inline const char *klog_find_nl(const char *s, size_t len) {
  for (size_t i = 0; i < len; i++)
    if (s[i] == '\n')
      return s + i;
  return NULL;
}

static void klog_emit(const char *s, size_t len) {
  if (!s || len == 0)
    return;
  hal_irq_state_t flags;
  struct klog_cpu_buf *b = klog_cpu_claim(&flags);
  if (!b) {
    klog_write_dispatch(s, len);
    return;
  }
  while (len) {
    /* A complete line with nothing pending goes out without a copy. */
    const char *nl = klog_find_nl(s, len);
    size_t seg = nl ? (size_t)(nl - s) + 1 : len;
    if (b->len == 0 && nl) {
      klog_write_dispatch(s, seg);
    } else {
      size_t off = 0;
      while (off < seg) {
        size_t room = KLOG_LINE_SIZE - b->len;
        size_t n = seg - off;
        if (n > room)
          n = room;
        memcpy(&b->line[b->len], s + off, n);
        b->len += (uint32_t)n;
        off += n;
        if (b->len == KLOG_LINE_SIZE)
          klog_line_flush_locked(b);
      }
      if (nl)
        klog_line_flush_locked(b);
    }
    s += seg;
    len -= seg;
  }
  klog_cpu_unclaim(b, flags);
}

/* Push out any partial line still buffered.  This CPU's buffer is flushed
 * safely; with `all_cpus` (panic/halt paths only) other CPUs' idle buffers are
 * flushed too - racy by nature, but those CPUs are stopped or about to be. */
void klog_flush(bool all_cpus) {
  hal_irq_state_t flags;
  struct klog_cpu_buf *b = klog_cpu_claim(&flags);
  if (b) {
    klog_line_flush_locked(b);
    klog_cpu_unclaim(b, flags);
  }
  if (!all_cpus)
    return;
  for (int i = 0; i < MAX_CPUS; i++) {
    struct klog_cpu_buf *o = &klog_pcpu[i];
    if (o == b || o->busy || o->len == 0)
      continue;
    uint32_t n = o->len;
    o->len = 0; /* before dispatch: see klog_line_flush_locked */
    klog_write_dispatch(o->line, n);
  }
}

void klog_putchar(char c) { klog_emit(&c, 1); }

void klog_puts(const char *s) {
  if (!s)
    return;

  size_t len = 0;
  while (s[len])
    len++;

  klog_emit(s, len);
}

void klog_uint64(uint64_t num) {
  char buf[24];
  if (num == 0) {
    buf[0] = '0';
    buf[1] = '\0';
    klog_emit(buf, 1);
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

  klog_emit(buf, (size_t)len);
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

  klog_emit(buf, 18);
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

  klog_emit(buf, 10);
}

void vklogf(const char *fmt, va_list ap) {
  if (!fmt)
    return;

  hal_irq_state_t flags;
  struct klog_cpu_buf *b = klog_cpu_claim(&flags);
  if (!b) {
    /* Re-entrant (fault inside klog) or pre-SMP oddity: small stack buffer. */
    char small[192];
    int len = vsnprintf(small, sizeof(small), fmt, ap);
    if (len <= 0)
      return;
    if ((size_t)len >= sizeof(small))
      len = (int)sizeof(small) - 1;
    klog_write_dispatch(small, (size_t)len);
    return;
  }

  int len = vsnprintf(b->fmt, sizeof(b->fmt), fmt, ap);
  if (len > 0) {
    if ((size_t)len >= sizeof(b->fmt))
      len = (int)sizeof(b->fmt) - 1;
    /* Release the claim first so klog_emit can append to the line buffer
     * (keeping any pending klog_puts prefix in front of this text).  IRQs stay
     * off, so b->fmt cannot be reused by this CPU before it is consumed. */
    b->busy = 0;
    klog_emit(b->fmt, (size_t)len);
    hal_irq_restore(flags);
    return;
  }
  klog_cpu_unclaim(b, flags);
}

void klogf(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vklogf(fmt, ap);
  va_end(ap);
}

extern uint64_t lapic_timer_get_ms(void);

bool klog_ratelimit_check(struct klog_ratelimit *rl, const char *tag,
                          uint32_t burst, uint32_t interval_ms) {
  uint64_t now = lapic_timer_get_ms();
  uint64_t start = __atomic_load_n(&rl->window_start_ms, __ATOMIC_RELAXED);

  /* First use, or the window expired: one CPU wins the reset and reports what
   * the previous window suppressed. */
  if (start == 0 || now - start >= interval_ms) {
    if (__atomic_compare_exchange_n(&rl->window_start_ms, &start,
                                    now ? now : 1, false, __ATOMIC_RELAXED,
                                    __ATOMIC_RELAXED)) {
      uint32_t missed = __atomic_exchange_n(&rl->missed, 0, __ATOMIC_RELAXED);
      __atomic_store_n(&rl->printed, 0, __ATOMIC_RELAXED);
      if (missed)
        klogf("[klog] %s: %u messages suppressed\n", tag ? tag : "?", missed);
    }
  }

  if (__atomic_add_fetch(&rl->printed, 1, __ATOMIC_RELAXED) <= burst)
    return true;
  __atomic_add_fetch(&rl->missed, 1, __ATOMIC_RELAXED);
  return false;
}

bool klog_path_is_browser(const char *path) {
  return path && (strstr(path, "badwolf") || strstr(path, "bwrap") ||
                  strstr(path, "WebKit") || strstr(path, "webkit") ||
                  strstr(path, "mocktail") || strstr(path, "MiniBrowser"));
}

bool klog_path_is_sddm(const char *path) {
  return path && (strstr(path, "sddm") || strstr(path, "Xorg") ||
                  strstr(path, "sddm-greeter") || strstr(path, "sddm-helper"));
}

void klog_proc_exec(uint32_t tid, const char *path) {
  if (klog_path_is_browser(path)) {
    char note[160];
    int nlen = snprintf(note, sizeof(note), "[PROC] [WEBKIT/BROWSER] Launching %s (tid=%u)\n",
                        path, tid);
    if (nlen > 0)
      klog_emit(note, (size_t)nlen);
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

  klog_emit(buf, (size_t)(p - buf));
#else
  (void)ptr;
  (void)capacity;
  (void)is_pmm;
  (void)pages;
#endif
}
