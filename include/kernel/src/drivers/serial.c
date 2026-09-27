#include "serial.h"
#include "../io/io.h"
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

// Drain up to 16 bytes (16550 FIFO size) into UART if transmit holding register is empty.
// Must be called while holding serial_lock.
static inline void serial_drain_fifo_locked(void) {
  if (!is_transmit_empty())
    return;

  int max_drain = 16;
  uint32_t tail = serial_tail;
  uint32_t head = serial_head;

  while (tail != head && max_drain-- > 0) {
    outb(COM1, serial_buf[tail]);
    tail = (tail + 1) & SERIAL_BUF_MASK;
  }
  serial_tail = tail;
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

/* How much is queued but not yet on the wire.  The ring is drained only by the
 * BSP's timer tick, so a nonzero count that keeps growing means the BSP has
 * stopped ticking - which would make the log stop mid-sentence on a kernel that
 * is otherwise still running, and is the first thing to rule out when a hang
 * report never appears. */
uint32_t serial_pending_bytes(void) {
  uint32_t head = __atomic_load_n(&serial_head, __ATOMIC_RELAXED);
  uint32_t tail = __atomic_load_n(&serial_tail, __ATOMIC_RELAXED);
  return (uint32_t)((head - tail) & SERIAL_BUF_MASK);
}

// Drain as many queued bytes as the UART FIFO can accept right now (non-blocking)
void serial_flush(void) {
  /* Called from the BSP timer tick: never block, or a contended log lock
   * stops this CPU from taking ticks and the hang detector fires. */
  if (!spinlock_try_acquire(&serial_lock))
    return;
  serial_drain_fifo_locked();
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

    int max_drain = 16;
    uint32_t tail = serial_tail;
    uint32_t head = serial_head;
    while (tail != head && max_drain-- > 0) {
      outb(COM1, serial_buf[tail]);
      tail = (tail + 1) & SERIAL_BUF_MASK;
    }
    serial_tail = tail;
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
  serial_drain_fifo_locked();
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

  uint32_t head = serial_head;
  uint32_t tail = serial_tail;
  uint32_t free_space = (tail - head - 1) & SERIAL_BUF_MASK;

  for (size_t i = 0; i < length; i++) {
    if (free_space == 0)
      break; // Buffer full: drop remainder without port I/O stall

    char c = data[i];
    if (c == '\n') {
      if (free_space < 2)
        break;
      serial_buf[head] = '\r';
      head = (head + 1) & SERIAL_BUF_MASK;
      free_space--;
    }

    serial_buf[head] = c;
    head = (head + 1) & SERIAL_BUF_MASK;
    free_space--;
  }

  serial_head = head;

  while (is_transmit_empty() && serial_tail != serial_head) {
    int max_drain = 16;
    uint32_t t = serial_tail;
    uint32_t h = serial_head;
    while (t != h && max_drain-- > 0) {
      outb(COM1, serial_buf[t]);
      t = (t + 1) & SERIAL_BUF_MASK;
    }
    serial_tail = t;
  }

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
