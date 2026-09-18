#include "serial.h"
#include "../console/klog.h"
#include "../cpu/tsc.h"
#include "../io/io.h"
#include "../lib/string.h"
#include "../lib/tsc.h"
#include "../lock/spinlock.h"

#define COM1 0x3F8

// Ring buffer for non-blocking serial output (128KB power-of-2 size)
#define SERIAL_BUF_SIZE 131072
#define SERIAL_BUF_MASK (SERIAL_BUF_SIZE - 1)

static char serial_buf[SERIAL_BUF_SIZE];
static volatile uint32_t serial_head = 0; // write position
static volatile uint32_t serial_tail = 0; // read/send position

static spinlock_t serial_lock = SPINLOCK_INIT;
static volatile bool serial_initialized = false;

static inline int is_transmit_empty(void) { return inb(COM1 + 5) & 0x20; }

/* Push a burst with one string I/O instead of one port access per byte: under
 * KVM every OUT is a VM exit, and the console mirror plus klog push thousands
 * of characters through here.  Callers gate each burst on the transmit
 * register so the 16550 FIFO is never overrun. */
static inline void serial_out_block(const char *buf, uint32_t count) {
  if (!count)
    return;
  __asm__ volatile("rep outsb"
                   : "+S"(buf), "+c"(count)
                   : "d"((uint16_t)COM1)
                   : "memory");
}

/* Lock-free, non-blocking fallback for when serial_lock is already held by
 * this CPU (a fault inside a logging section) or by a CPU that cannot make
 * progress.  Dropping bytes is always better than deadlocking the machine;
 * this is only reached on contention. */
static inline void serial_try_putbyte(char c) {
  if (is_transmit_empty())
    outb(COM1, c);
}

/* Bounded wait for serial_lock.  Normal contention (another CPU mid-drain)
 * resolves in microseconds, so this keeps concurrent reports serialized.
 * Only a genuinely wedged holder falls through to the lock-free path. */
#define SERIAL_LOCK_SPIN_LIMIT 100000u
static inline bool serial_lock_bounded(void) {
  for (uint32_t i = 0; i < SERIAL_LOCK_SPIN_LIMIT; i++) {
    if (spinlock_try_acquire(&serial_lock))
      return true;
    __asm__ volatile("pause" ::: "memory");
  }
  return false;
}

void serial_init(void) {
  spinlock_acquire(&serial_lock);

  outb(COM1 + 1, 0x00); // Disable all interrupts
  outb(COM1 + 3, 0x80); // Enable DLAB (set baud rate divisor)
  outb(COM1 + 0, 0x01); // Set divisor to 1 (lo byte) 115200 baud
  outb(COM1 + 1, 0x00); //                  (hi byte)
  outb(COM1 + 3, 0x03); // 8 bits, no parity, one stop bit
  outb(COM1 + 2, 0xC7); // Enable FIFO, clear them, with 14-byte threshold
  outb(COM1 + 4, 0x0B); // IRQs enabled, RTS/DSR set

  serial_initialized = true;

  spinlock_release(&serial_lock);
}

/* A single THRE check covers at most one 16550 FIFO (16 bytes); a real UART
 * only takes another burst once that FIFO has drained, so on hardware one
 * check per burst is exactly right.  An emulated UART (KVM) reports room
 * immediately, which turns "one burst per drain point" into a hard 16 bytes
 * per call - 16 KB/s on the 1 ms timer tick, far below what the device model
 * accepts.  A drain point therefore moves bytes until the UART stops
 * accepting them or its budget runs out.  The budget only ever binds on an
 * always-ready transmit register; hardware still stops after the first FIFO. */
#define SERIAL_FIFO_BYTES 16

/* Bytes one drain point may move, per call site:
 *  - timer tick: bounded, an ISR must not spend the whole tick feeding COM1;
 *  - console mirror: one rendered batch, so the mirror tracks the framebuffer
 *    instead of leaving the rest of the batch in the ring;
 *  - idle loop: the BSP has nothing else to run, so a larger slice of the
 *    backlog can go out at the device's own pace (bounded so console_tick()
 *    and the hang detector still get a turn between slices). */
#define SERIAL_TICK_DRAIN_BYTES 32u
#define SERIAL_ASYNC_DRAIN_BYTES 128u
#define SERIAL_IDLE_DRAIN_BYTES 512u

/* Move up to `budget` bytes from the ring to the UART, one FIFO burst per
 * THRE check, stopping as soon as the UART stops accepting.  Caller holds
 * serial_lock.  Returns the number of bytes moved. */
static uint32_t serial_drain_locked(uint32_t budget) {
  uint32_t tail = serial_tail;
  uint32_t head = serial_head;
  uint32_t moved = 0;

  while (moved < budget && tail != head) {
    if (!is_transmit_empty())
      break;

    uint32_t chunk = (head - tail) & SERIAL_BUF_MASK;
    if (chunk > SERIAL_FIFO_BYTES)
      chunk = SERIAL_FIFO_BYTES;
    if (chunk > budget - moved)
      chunk = budget - moved;

    uint32_t to_end = SERIAL_BUF_SIZE - tail;
    uint32_t first = chunk < to_end ? chunk : to_end;
    serial_out_block(&serial_buf[tail], first);
    if (chunk > first)
      serial_out_block(&serial_buf[0], chunk - first);

    tail = (tail + chunk) & SERIAL_BUF_MASK;
    moved += chunk;
  }
  serial_tail = tail;
  return moved;
}

// Enqueue a character into the ring buffer (pure memory operation, 0 port I/O).
// Must be called while holding serial_lock.
static inline void serial_enqueue_locked(char c) {
  uint32_t next = (serial_head + 1) & SERIAL_BUF_MASK;
  if (next == serial_tail)
    return; // Buffer full: drop silently without blocking or port I/O
  serial_buf[serial_head] = c;
  serial_head = next;
}

/* How much is queued but not yet on the wire.  A nonzero count that keeps
 * growing means every drain point has stopped making progress - the timer
 * tick (serial_flush), the console mirror (serial_write_async) and the idle
 * pump (serial_flush_idle) all push bytes - which would make the log stop
 * mid-sentence on a kernel that is otherwise still running, and is the first
 * thing to rule out when a hang report never appears. */
uint32_t serial_pending_bytes(void) {
  uint32_t head = __atomic_load_n(&serial_head, __ATOMIC_RELAXED);
  uint32_t tail = __atomic_load_n(&serial_tail, __ATOMIC_RELAXED);
  return (uint32_t)((head - tail) & SERIAL_BUF_MASK);
}

/* Non-blocking drain for the BSP timer tick: bounded so a backlog cannot turn
 * the tick handler into a serial pump.  The idle loop and the console mirror
 * move the bulk; this keeps progress while the BSP is busy. */
void serial_flush(void) {
  /* Called from the BSP timer tick: never block, or a contended log lock
   * stops this CPU from taking ticks and the hang detector fires. */
  if (!spinlock_try_acquire(&serial_lock))
    return;
  serial_drain_locked(SERIAL_TICK_DRAIN_BYTES);
  spinlock_release(&serial_lock);
}

/* Idle-loop pump: the BSP has nothing else to run, so a chunk of the backlog
 * can go out at the device's own pace.  Never blocks on the lock (another CPU
 * mid-drain will pick the bytes up) and never waits for the line: on hardware
 * the first THRE check that fails ends the slice. */
void serial_flush_idle(void) {
  if (serial_pending_bytes() == 0)
    return;
  if (!spinlock_try_acquire(&serial_lock))
    return;
  serial_drain_locked(SERIAL_IDLE_DRAIN_BYTES);
  spinlock_release(&serial_lock);
}

// Synchronously drain the entire ring buffer until empty.
// Safe for panics and shutdown; includes timeout to prevent infinite hangs.
void serial_flush_sync(void) {
  if (!spinlock_try_acquire(&serial_lock)) {
    /* Panic on the CPU that holds serial_lock: draining lock-free is the only
     * way out.  Racing another drain can drop bytes; that is fine here. */
    uint32_t guard = 0;
    while (serial_tail != serial_head && guard++ < SERIAL_BUF_SIZE) {
      serial_putchar_sync(serial_buf[serial_tail]);
      serial_tail = (serial_tail + 1) & SERIAL_BUF_MASK;
    }
    return;
  }

  while (serial_tail != serial_head) {
    uint32_t timeout = 1000000;
    while (!is_transmit_empty() && --timeout > 0) {
      __asm__ volatile("pause" ::: "memory");
    }
    if (timeout == 0) {
      // Hardware unresponsive, drop remaining buffer to avoid hang
      break;
    }

    serial_drain_locked(SERIAL_FIFO_BYTES);
  }

  spinlock_release(&serial_lock);
}

void serial_putchar(char c) {
  if (!serial_lock_bounded()) {
    if (c == '\n')
      serial_try_putbyte('\r');
    serial_try_putbyte(c);
    return;
  }
  if (c == '\n')
    serial_enqueue_locked('\r');
  serial_enqueue_locked(c);
  serial_drain_locked(SERIAL_FIFO_BYTES);
  spinlock_release(&serial_lock);
}

/* Enqueue a byte range with LF->CRLF translation, touching no port.  The
 * caller holds serial_lock.  Returns the number of bytes queued; bytes that do
 * not fit are dropped. */
static uint32_t serial_enqueue_range_locked(const char *data, size_t length) {
  uint32_t head = serial_head;
  uint32_t free_space = (serial_tail - head - 1) & SERIAL_BUF_MASK;
  uint32_t queued = 0;

  for (size_t i = 0; i < length; i++) {
    if (free_space == 0)
      break;

    char c = data[i];
    if (c == '\n') {
      if (free_space < 2)
        break;
      serial_buf[head] = '\r';
      head = (head + 1) & SERIAL_BUF_MASK;
      free_space--;
      queued++;
    }

    serial_buf[head] = c;
    head = (head + 1) & SERIAL_BUF_MASK;
    free_space--;
    queued++;
  }

  serial_head = head;
  return queued;
}

/* Best-effort mirror write used by the framebuffer console: queue the bytes
 * and move one rendered batch (128 B) while the UART accepts, but never wait
 * for the line.  Draining the whole write synchronously (what serial_write
 * does) throttles the console to the UART rate - ~11.5 KB/s on a real 16550
 * at 115200 baud.  An emulated UART keeps reporting room, so without a budget
 * this would stall the console for milliseconds per batch; with the budget the
 * mirror keeps pace with the framebuffer instead of leaving the batch queued
 * for the tick.  Overflow is dropped: the framebuffer is the authoritative
 * output, serial is a mirror. */
void serial_write_async(const char *data, size_t length) {
  if (!data || length == 0)
    return;

  if (!serial_lock_bounded())
    return; // Mirror only: never fall back to direct port I/O.

  serial_enqueue_range_locked(data, length);
  serial_drain_locked(SERIAL_ASYNC_DRAIN_BYTES);
  spinlock_release(&serial_lock);
}

void serial_write(const char *data, size_t length) {
  if (!data || length == 0)
    return;

  if (!serial_lock_bounded()) {
    for (size_t i = 0; i < length; i++) {
      if (data[i] == '\n')
        serial_try_putbyte('\r');
      serial_try_putbyte(data[i]);
    }
    return;
  }

  serial_enqueue_range_locked(data, length);

  /* klog writes are synchronous by contract: push until the UART stops
   * accepting (a real 16550 fills its FIFO after 16 bytes). */
  serial_drain_locked(UINT32_MAX);

  spinlock_release(&serial_lock);
}

// Direct synchronous write bypassing the ring buffer (ideal for early boot or panic)
void serial_putchar_sync(char c) {
  if (c == '\n')
    serial_putchar_sync('\r');

  uint32_t timeout = 1000000;
  while (!is_transmit_empty() && --timeout > 0) {
    __asm__ volatile("pause" ::: "memory");
  }
  outb(COM1, c);
}

void serial_write_sync(const char *data, size_t length) {
  if (!data || length == 0)
    return;
  for (size_t i = 0; i < length; i++) {
    serial_putchar_sync(data[i]);
  }
}

int serial_received(void) { return inb(COM1 + 5) & 1; }

/* Non-blocking read: -1 when nothing is waiting.  Used by the hang-report
 * trigger, which polls from the timer tick and must never stall. */
int serial_try_get_char(void) {
  if (!(inb(COM1 + 5) & 1))
    return -1;
  return (int)(uint8_t)inb(COM1);
}

char serial_get_char(void) {
  while (serial_received() == 0) {
    __asm__ volatile("pause" ::: "memory");
  }
  return inb(COM1);
}

/* ── `serial_bench=1`: COM1 drain-path benchmark ─────────────────────────────
 *
 * The driver's throughput is decided by how much a single drain point moves,
 * not by the port I/O itself: the timer tick, each console mirror batch and
 * the idle-loop pump are the only chances the UART gets.  This measures those
 * paths on the real device - one serial_flush() per simulated tick,
 * serial_write_async() per console batch, serial_flush_idle() until the
 * backlog is gone, one serial_write() for a klog burst - plus the raw cost of
 * a 16-byte FIFO burst against the legacy one-OUT-per-byte form.  Compiled in
 * and gated on the kernel command line exactly like fb_bench/drm_bench; it
 * runs once early, before the first boot message, and leaves the ring empty.
 */

#define COM1_BENCH_FILL_BYTES 16384u
#define COM1_BENCH_MIRROR_TOTAL 16384u
#define COM1_BENCH_MIRROR_BATCH 128u
#define COM1_BENCH_SYNC_BYTES 2048u
#define COM1_BENCH_PORT_BYTES 1024u
#define COM1_BENCH_TIMEOUT_MS 400u
#define COM1_BENCH_IDLE_TIMEOUT_MS 1000u

extern const char *kernel_boot_cmdline;

/* Fill the ring with `bytes` of a marked pattern, bypassing CRLF translation
 * and the port.  Callers run on the BSP before any other writer exists. */
static uint32_t bench_fill(uint32_t bytes) {
  uint32_t n = 0;

  serial_tail = 0;
  serial_head = 0;
  while (n < bytes) {
    uint32_t col = 0;
    while (col < 63 && n < bytes) {
      serial_buf[n++] = '=';
      col++;
    }
    if (n < bytes)
      serial_buf[n++] = '\n';
  }
  serial_head = n;
  return n;
}

static void bench_report(const char *name, uint32_t bytes, uint32_t calls,
                         uint64_t cycles) {
  uint64_t ns = tsc_cycles_to_ns(cycles);

  klog_puts("[COM1-BENCH] ");
  klog_puts(name);
  klog_puts(": ");
  klog_uint64(bytes);
  klog_puts(" B, ");
  klog_uint64(calls);
  klog_puts(calls == 1 ? " call, " : " calls, ");
  klog_uint64(calls ? bytes / calls : 0);
  klog_puts(" B/call");
  if (ns) {
    klog_puts(", ");
    klog_uint64(ns / 1000);
    klog_puts(" us, ");
    /* bytes / ns * 1e9 / 1000 = bytes * 1e6 / ns (KB/s) */
    klog_uint64((uint64_t)bytes * 1000000ULL / ns);
    klog_puts(" KB/s");
  } else {
    klog_puts(", ");
    klog_uint64(cycles);
    klog_puts(" cycles");
  }
  klog_puts("\n");
}

/* The old tick policy in one call: THRE check, then at most one FIFO burst -
 * exactly what serial_flush() did before the drain points were budgeted. */
static void bench_old_flush(void) {
  if (!spinlock_try_acquire(&serial_lock))
    return;
  if (is_transmit_empty())
    serial_drain_locked(SERIAL_FIFO_BYTES);
  spinlock_release(&serial_lock);
}

/* Old vs new tick drain on the same filled ring, back to back, the way
 * drm_bench_pair() contrasts the old bounding box with the clip list: the
 * only difference is how many bytes one drain point moves. */
static void bench_tick_pair(void) {
  const uint32_t bytes = COM1_BENCH_FILL_BYTES;
  uint64_t deadline = tsc_get_freq_khz() * COM1_BENCH_TIMEOUT_MS;
  uint32_t old_calls = 0;
  uint32_t new_calls = 0;
  uint32_t old_drained = 0;
  uint32_t new_drained = 0;
  uint64_t old_cycles;
  uint64_t new_cycles;

  serial_flush_sync();
  bench_fill(bytes);
  uint64_t t0 = rdtsc_fence();
  while (serial_pending_bytes() && old_calls < bytes) {
    bench_old_flush();
    old_calls++;
    if (deadline && rdtsc() - t0 > deadline)
      break;
  }
  old_cycles = rdtsc_fence() - t0;
  old_drained = bytes - serial_pending_bytes();
  serial_flush_sync();

  bench_fill(bytes);
  t0 = rdtsc_fence();
  while (serial_pending_bytes() && new_calls < bytes) {
    serial_flush();
    new_calls++;
    if (deadline && rdtsc() - t0 > deadline)
      break;
  }
  new_cycles = rdtsc_fence() - t0;
  new_drained = bytes - serial_pending_bytes();
  serial_flush_sync();

  bench_report("tick-old ", old_drained, old_calls, old_cycles);
  bench_report("tick-new ", new_drained, new_calls, new_cycles);

  /* One tick per ms, so a byte per tick is a KB/s (1000 ticks/s). */
  klog_puts("[COM1-BENCH] tick at 1 ms: old ");
  klog_uint64(old_calls ? old_drained / old_calls : 0);
  klog_puts(" KB/s, new ");
  klog_uint64(new_calls ? new_drained / new_calls : 0);
  klog_puts(" KB/s\n");
}

/* What the idle-loop pump moves: the BSP has nothing else to run, so slices
 * of the backlog go out at the device's own pace.  On hardware each slice
 * ends at the first full FIFO; the number here is the emulated-UART ceiling. */
static void bench_idle_drain(void) {
  uint32_t queued = bench_fill(COM1_BENCH_FILL_BYTES);
  uint64_t deadline = tsc_get_freq_khz() * COM1_BENCH_IDLE_TIMEOUT_MS;
  uint32_t calls = 0;
  uint64_t t0 = rdtsc_fence();

  while (serial_pending_bytes()) {
    serial_flush_idle();
    calls++;
    if (calls > queued || (deadline && rdtsc() - t0 > deadline))
      break;
  }

  uint64_t cycles = rdtsc_fence() - t0;
  uint32_t drained = queued - serial_pending_bytes();

  bench_report("idle-drain", drained, calls, cycles);
  serial_flush_sync();
}

/* How much one console mirror batch delivers: the console calls
 * serial_write_async() once per rendered batch and never waits on the line.
 * Reports the bytes that actually made it out during the producer burst; the
 * backlog is the mirror lag. */
static void bench_mirror_push(void) {
  static char batch[COM1_BENCH_MIRROR_BATCH];
  uint32_t pushed = 0;
  uint32_t calls = 0;

  for (uint32_t i = 0; i < sizeof(batch); i++)
    batch[i] = '+';

  serial_flush_sync();
  uint64_t t0 = rdtsc_fence();
  while (pushed < COM1_BENCH_MIRROR_TOTAL) {
    serial_write_async(batch, sizeof(batch));
    pushed += sizeof(batch);
    calls++;
  }
  uint64_t cycles = rdtsc_fence() - t0;

  uint32_t pending = serial_pending_bytes();
  uint32_t delivered = pushed > pending ? pushed - pending : 0;

  bench_report("mirror-push", delivered, calls, cycles);
  klog_puts("[COM1-BENCH] mirror-push: pushed ");
  klog_uint64(pushed);
  klog_puts(" B, backlog left ");
  klog_uint64(pending);
  klog_puts(" B\n");
  serial_flush_sync();
}

/* Cost of a full synchronous klog burst through serial_write(). */
static void bench_sync_write(void) {
  static char buf[COM1_BENCH_SYNC_BYTES];

  for (uint32_t i = 0; i < sizeof(buf); i++)
    buf[i] = (i % 63 == 62) ? '\n' : '-';

  serial_flush_sync();
  uint64_t t0 = rdtsc_fence();
  serial_write(buf, sizeof(buf));
  uint64_t cycles = rdtsc_fence() - t0;

  bench_report("sync-write", COM1_BENCH_SYNC_BYTES, 1, cycles);
}

/* Raw port cost: THRE check + one 16-byte rep outsb versus THRE check + one
 * OUT per byte.  This is what the drain burst is built from. */
static void bench_port_costs(void) {
  static const char burst[17] = "0123456789ABCDEF";

  uint64_t t0 = rdtsc_fence();
  for (uint32_t i = 0; i < COM1_BENCH_PORT_BYTES / 16; i++) {
    while (!is_transmit_empty()) {
    }
    serial_out_block(burst, 16);
  }
  uint64_t burst_cycles = rdtsc_fence() - t0;

  t0 = rdtsc_fence();
  for (uint32_t i = 0; i < COM1_BENCH_PORT_BYTES; i++) {
    while (!is_transmit_empty()) {
    }
    outb(COM1, (uint8_t)burst[i & 15]);
  }
  uint64_t byte_cycles = rdtsc_fence() - t0;

  bench_report("burst16  ", COM1_BENCH_PORT_BYTES, COM1_BENCH_PORT_BYTES / 16,
               burst_cycles);
  bench_report("bytewise ", COM1_BENCH_PORT_BYTES, COM1_BENCH_PORT_BYTES,
               byte_cycles);
}

void serial_bench_maybe_run(void) {
  if (!kernel_boot_cmdline || !strstr(kernel_boot_cmdline, "serial_bench"))
    return;

  klog_puts("[COM1-BENCH] COM1 drain benchmark: ring ");
  klog_uint64(SERIAL_BUF_SIZE / 1024);
  klog_puts(" KB, FIFO 16 B, tick drain measured back to back\n");

  serial_flush_sync();

  bench_tick_pair();
  bench_mirror_push();
  bench_idle_drain();
  bench_sync_write();
  bench_port_costs();

  serial_flush_sync();
  klog_puts("[COM1-BENCH] done\n");
}
