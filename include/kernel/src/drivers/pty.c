// PTY (Pseudoterminal) Driver
// Implements POSIX-style PTY multiplexor (/dev/ptmx) and slave devices
// (/dev/pts/N) for terminal emulators and shell support.

#include "pty.h"
#include "../console/klog.h"
#include "../fb/framebuffer.h"
#include "../fs/vfs.h"
#include "../lib/string.h"
#include "../mm/heap.h"
#include "../mm/pmm.h"
#include "../mm/vmm.h"
#include "../sched/sched.h"
#include "../sched/wait.h"
#include "../socket/epoll.h"
#include "../syscalls/syscall.h"
#include "arch/uaccess.h"

// PTY pair pool
static pty_pair_t pty_pool[PTY_MAX_PAIRS];
static int pty_next_index = 0;

// Ring buffer helpers

static inline uint32_t ring_used(uint32_t head, uint32_t tail) {
  return (head >= tail) ? (head - tail) : (PTY_BUFFER_SIZE - tail + head);
}

static inline uint32_t ring_free(uint32_t head, uint32_t tail) {
  return PTY_BUFFER_SIZE - ring_used(head, tail) - 1;
}

static inline bool ring_empty(uint32_t head, uint32_t tail) {
  return head == tail;
}

// Write to ring buffer, returns bytes written
static inline bool ring_full(uint32_t head, uint32_t tail) {
  return ((head + 1) % PTY_BUFFER_SIZE) == tail;
}

static uint32_t ring_write(uint8_t *buffer, uint32_t *head, uint32_t tail,
                           const uint8_t *data, uint32_t len) {
  uint32_t free_space = ring_free(*head, tail);
  if (free_space == 0)
    return 0;

  uint32_t to_write = (len < free_space) ? len : free_space;
  uint32_t first_part = PTY_BUFFER_SIZE - *head;

  if (first_part > to_write)
    first_part = to_write;

  if (is_user_ptr((uint64_t)data)) {
    unsigned long u1 = copy_from_user(buffer + *head, data, first_part);
    if (u1 > 0) {
      uint32_t written = first_part - (uint32_t)u1;
      *head = (*head + written) % PTY_BUFFER_SIZE;
      return written;
    }
    if (to_write > first_part) {
      unsigned long u2 = copy_from_user(buffer, data + first_part, to_write - first_part);
      if (u2 > 0) {
        uint32_t written = (to_write - first_part) - (uint32_t)u2;
        *head = (*head + first_part + written) % PTY_BUFFER_SIZE;
        return first_part + written;
      }
    }
  } else {
    memcpy(buffer + *head, data, first_part);
    if (to_write > first_part) {
      memcpy(buffer, data + first_part, to_write - first_part);
    }
  }

  *head = (*head + to_write) % PTY_BUFFER_SIZE;
  return to_write;
}

// Read from ring buffer, returns bytes read
static uint32_t ring_read(uint8_t *buffer, uint32_t head, uint32_t *tail,
                          uint8_t *data, uint32_t len) {
  uint32_t used = ring_used(head, *tail);
  if (used == 0)
    return 0;

  uint32_t to_read = (len < used) ? len : used;
  uint32_t first_part = PTY_BUFFER_SIZE - *tail;

  if (first_part > to_read)
    first_part = to_read;

  if (data) {
    if (is_user_ptr((uint64_t)data)) {
      unsigned long u1 = copy_to_user(data, buffer + *tail, first_part);
      if (u1 > 0) {
        uint32_t read_bytes = first_part - (uint32_t)u1;
        *tail = (*tail + read_bytes) % PTY_BUFFER_SIZE;
        return read_bytes;
      }
      if (to_read > first_part) {
        unsigned long u2 = copy_to_user(data + first_part, buffer, to_read - first_part);
        if (u2 > 0) {
          uint32_t read_bytes = (to_read - first_part) - (uint32_t)u2;
          *tail = (*tail + first_part + read_bytes) % PTY_BUFFER_SIZE;
          return first_part + read_bytes;
        }
      }
    } else {
      memcpy(data, buffer + *tail, first_part);
      if (to_read > first_part) {
        memcpy(data + first_part, buffer, to_read - first_part);
      }
    }
  }

  *tail = (*tail + to_read) % PTY_BUFFER_SIZE;
  return to_read;
}

// PTY subsystem initialization

void pty_init(void) {
  static bool pty_initialized = false;
  if (pty_initialized)
    return;
  pty_initialized = true;

  for (int i = 0; i < PTY_MAX_PAIRS; i++) {
    pty_pool[i].index = i;
    pty_pool[i].allocated = false;
    pty_pool[i].locked = false;
    pty_pool[i].master_open = false;
    pty_pool[i].packet_mode = false;
    pty_pool[i].master_node = NULL;
    pty_pool[i].slave_open_count = 0;
    pty_pool[i].m2s_head = 0;
    pty_pool[i].m2s_tail = 0;
    pty_pool[i].m2s_newline_count = 0;
    pty_pool[i].s2m_head = 0;
    pty_pool[i].s2m_tail = 0;
    pty_pool[i].pgid = 0;
    if (!pty_pool[i].master_waitq)
      pty_pool[i].master_waitq = kmalloc(sizeof(wait_queue_t));
    if (!pty_pool[i].slave_waitq)
      pty_pool[i].slave_waitq = kmalloc(sizeof(wait_queue_t));
    if (!pty_pool[i].slave_write_waitq)
      pty_pool[i].slave_write_waitq = kmalloc(sizeof(wait_queue_t));
    if (pty_pool[i].master_waitq)
      wait_queue_init((wait_queue_t *)pty_pool[i].master_waitq);
    if (pty_pool[i].slave_waitq)
      wait_queue_init((wait_queue_t *)pty_pool[i].slave_waitq);
    if (pty_pool[i].slave_write_waitq)
      wait_queue_init((wait_queue_t *)pty_pool[i].slave_write_waitq);
    spinlock_init(&pty_pool[i].lock);

    // Default terminal settings
    pty_pool[i].termios.c_iflag = 0x00000100; // ICRNL
    pty_pool[i].termios.c_oflag = 0x00000005; // OPOST | ONLCR
    pty_pool[i].termios.c_cflag = 0;
    pty_pool[i].termios.c_lflag = 0x0000000b; // ISIG | ICANON | ECHO
    pty_pool[i].termios.c_line = 0;

    // POSIX standard control character defaults
    for (int j = 0; j < NCCS; j++)
      pty_pool[i].termios.c_cc[j] = 0;
    pty_pool[i].termios.c_cc[0] = 0x03; // VINTR (Ctrl-C)
    pty_pool[i].termios.c_cc[1] = 0x1C; // VQUIT (Ctrl-\)
    pty_pool[i].termios.c_cc[2] = 0x7F; // VERASE (Backspace)
    pty_pool[i].termios.c_cc[3] = 0x15; // VKILL (Ctrl-U)
    pty_pool[i].termios.c_cc[4] = 0x04; // VEOF (Ctrl-D)

    // Window size (standard desktop-ish defaults)
    pty_pool[i].winsize.ws_row = 24;
    pty_pool[i].winsize.ws_col = 80;
    pty_pool[i].winsize.ws_xpixel = 80 * 8;
    pty_pool[i].winsize.ws_ypixel = 24 * 16;
  }

#if PTY_DEBUG_LOGGING
  klog_puts("[PTY] Initialized with 16 pairs\n");
#endif
}

// PTY allocation

int pty_alloc_pair(void) {
  pty_init();

  for (int i = 0; i < PTY_MAX_PAIRS; i++) {
    int idx = (pty_next_index + i) % PTY_MAX_PAIRS;
    if (!pty_pool[idx].allocated) {
      pty_pool[idx].allocated = true;
      pty_pool[idx].locked = false;
      pty_pool[idx].master_open = true;
      pty_pool[idx].packet_mode = false;
      pty_pool[idx].master_node = NULL;
      pty_pool[idx].slave_open_count = 0;
      pty_pool[idx].m2s_head = 0;
      pty_pool[idx].m2s_tail = 0;
      pty_pool[idx].m2s_newline_count = 0;
      pty_pool[idx].s2m_head = 0;
      pty_pool[idx].s2m_tail = 0;
      pty_pool[idx].pgid = 0;
      pty_pool[idx].shared_mmap_frame = NULL;

      // Reset terminal settings
      pty_pool[idx].termios.c_iflag = 0x00000100;
      pty_pool[idx].termios.c_oflag = 0x00000005;
      pty_pool[idx].termios.c_cflag = 0;
      pty_pool[idx].termios.c_lflag = 0x0000000b;

      if (!pty_pool[idx].master_waitq)
        pty_pool[idx].master_waitq = kmalloc(sizeof(wait_queue_t));
      if (!pty_pool[idx].slave_waitq)
        pty_pool[idx].slave_waitq = kmalloc(sizeof(wait_queue_t));
      if (!pty_pool[idx].slave_write_waitq)
        pty_pool[idx].slave_write_waitq = kmalloc(sizeof(wait_queue_t));

      if (pty_pool[idx].master_waitq)
        wait_queue_init((wait_queue_t *)pty_pool[idx].master_waitq);
      if (pty_pool[idx].slave_waitq)
        wait_queue_init((wait_queue_t *)pty_pool[idx].slave_waitq);
      if (pty_pool[idx].slave_write_waitq)
        wait_queue_init((wait_queue_t *)pty_pool[idx].slave_write_waitq);

      spinlock_init(&pty_pool[idx].lock);

      pty_next_index = (idx + 1) % PTY_MAX_PAIRS;

#if PTY_DEBUG_LOGGING
      klog_puts("[PTY] Allocated PTY pair ");
      klog_uint64(idx);
      klog_puts("\n");
#endif

      return idx;
    }
  }

#if PTY_DEBUG_LOGGING
  klog_puts("[PTY] No free PTY pairs\n");
#endif
  return -1; // ENOSPC
}

pty_pair_t *pty_get_pair(int index) {
  if (index < 0 || index >= PTY_MAX_PAIRS)
    return NULL;
  if (!pty_pool[index].allocated)
    return NULL;
  return &pty_pool[index];
}

// Data availability checks

bool pty_master_can_read(pty_pair_t *pty) {
  return !ring_empty(pty->s2m_head, pty->s2m_tail);
}

bool pty_slave_can_read(pty_pair_t *pty) {
  if (ring_empty(pty->m2s_head, pty->m2s_tail)) {
    return false;
  }

  if (pty->termios.c_lflag & ICANON) {
    // In canonical mode, data is only available if there's a newline
    return pty->m2s_newline_count > 0;
  }

  return true;
}

// /dev/ptmx (master) VFS operations

uint32_t ptmx_read(struct vfs_node *node, uint32_t offset, uint32_t size,
                   uint8_t *buffer) {
  (void)offset;

  pty_pair_t *pty = (pty_pair_t *)node->device;
  if (!pty)
    return 0;

  spinlock_acquire(&pty->lock);
  // Master reads from slave→master buffer
  while (ring_empty(pty->s2m_head, pty->s2m_tail)) {
    if (pty->slave_open_count == 0) {
      spinlock_release(&pty->lock);
      return 0; // EOF: No slaves left
    }

    struct thread *current = sched_get_current();
    bool is_nonblock = (node->flags & FS_NONBLOCK) != 0;
    if (!is_nonblock && current) {
      for (int fd = 0; fd < MAX_FDS; fd++) {
        if (current->fds[fd] == node && (current->fd_flags[fd] & 0x800)) {
          is_nonblock = true;
          break;
        }
      }
    }
    if (is_nonblock) {
      spinlock_release(&pty->lock);
      return (uint32_t)-11; /* EAGAIN */
    }

    // Wait for data from slave
    wait_queue_entry_t entry = {0};
    entry.thread = sched_get_current();
    wait_queue_add((wait_queue_t *)pty->master_waitq, &entry);
    entry.thread->state = THREAD_BLOCKED;

    // Last second check while BLOCKED
    if (!ring_empty(pty->s2m_head, pty->s2m_tail) ||
        pty->slave_open_count == 0) {
      entry.thread->state = THREAD_RUNNING;
      spinlock_release(&pty->lock);
    } else {
      spinlock_release(&pty->lock);
      sched_yield();
    }

    spinlock_acquire(&pty->lock);
    wait_queue_remove((wait_queue_t *)pty->master_waitq, &entry);
    entry.thread->state = THREAD_RUNNING;
  }

  uint32_t read;
  if (pty->packet_mode && size > 0) {
    buffer[0] = 0; /* TIOCPKT_DATA */
    read = 1 + ring_read(pty->slave_to_master, pty->s2m_head,
                         &pty->s2m_tail, buffer + 1, size - 1);
  } else {
    read = ring_read(pty->slave_to_master, pty->s2m_head, &pty->s2m_tail,
                     buffer, size);
  }

  // Wake up any writers waiting for buffer space
  if (read > 0 && pty->slave_write_waitq &&
      ((wait_queue_t *)pty->slave_write_waitq)->head != NULL) {
    wait_queue_wake_all(pty->slave_write_waitq);
  }

  spinlock_release(&pty->lock);
  return read;
}

uint32_t ptmx_write(struct vfs_node *node, uint32_t offset, uint32_t size,
                    uint8_t *buffer) {
  (void)offset;

  pty_pair_t *pty = (pty_pair_t *)node->device;
  if (!pty)
    return 0;

#if PTY_DEBUG_LOGGING
  klog_puts("[PTY] ptmx_write: size=");
  klog_uint64(size);
  klog_puts(" first_byte=0x");
  klog_hex64(buffer[0]);
  klog_puts("\n");
#endif

  bool wake_slave = false;
  bool wake_master = false;
  uint32_t written = 0;

  spinlock_acquire(&pty->lock);
#if PTY_DEBUG_LOGGING
  klog_puts("[PTY] ptmx_write: slave_open_count=");
  klog_uint64(pty->slave_open_count);
  klog_puts(" ICANON=");
  klog_uint64(pty->termios.c_lflag & ICANON ? 1 : 0);
  klog_puts(" ECHO=");
  klog_uint64(pty->termios.c_lflag & ECHO ? 1 : 0);
  klog_puts("\n");
#endif

  if (pty->slave_open_count == 0) {
    spinlock_release(&pty->lock);
#if PTY_DEBUG_LOGGING
    klog_puts("[PTY] ptmx_write: no slave open, returning 0\n");
#endif
    return 0; // Or return -EPIPE/-32
  }

  // Fast path for raw mode (no input/output processing, no echo)
  // Even in raw mode we must still check ISIG control characters.
  if (!(pty->termios.c_lflag & (ICANON | ECHO)) &&
      !(pty->termios.c_iflag & ICRNL)) {
    if (pty->termios.c_lflag & ISIG) {
      // Scan for signal characters before bulk-writing
      for (uint32_t i = 0; i < size; i++) {
        uint8_t c = buffer[i];
        if (c == pty->termios.c_cc[0] && pty->pgid != 0) { // VINTR -> SIGINT
          uint32_t pgid = pty->pgid;
          spinlock_release(&pty->lock);
          extern void signal_send_pgid(uint32_t pgid, int sig);
          signal_send_pgid(pgid, 2); // SIGINT
          return (uint32_t)size; // consumed
        }
        if (c == pty->termios.c_cc[1] && pty->pgid != 0) { // VQUIT -> SIGQUIT
          uint32_t pgid = pty->pgid;
          spinlock_release(&pty->lock);
          extern void signal_send_pgid(uint32_t pgid, int sig);
          signal_send_pgid(pgid, 3); // SIGQUIT
          return (uint32_t)size;
        }
      }
    }
    written = ring_write(pty->master_to_slave, &pty->m2s_head, pty->m2s_tail,
                         buffer, size);
    if (written > 0)
      wake_slave = true;
    spinlock_release(&pty->lock);
  } else {
    for (uint32_t i = 0; i < size; i++) {
      uint8_t c = buffer[i];

      // Input processing: ICRNL (map \r to \n)
      if (c == '\r' && (pty->termios.c_iflag & ICRNL)) {
        c = '\n';
      }

      // ISIG: check for signal-generating characters (VINTR, VQUIT)
      if (pty->termios.c_lflag & ISIG) {
        if (c == pty->termios.c_cc[0] && pty->pgid != 0) { // VINTR -> SIGINT
          uint32_t pgid = pty->pgid;
          spinlock_release(&pty->lock);
#if PTY_DEBUG_LOGGING
          klog_puts("[PTY] ISIG: sending SIGINT to pgid=");
          klog_uint64(pgid);
          klog_puts("\n");
#endif
          extern void signal_send_pgid(uint32_t pgid, int sig);
          signal_send_pgid(pgid, 2); // SIGINT
          return (uint32_t)(i + 1); // consumed up to and including this char
        }
        if (c == pty->termios.c_cc[1] && pty->pgid != 0) { // VQUIT -> SIGQUIT
          uint32_t pgid = pty->pgid;
          spinlock_release(&pty->lock);
#if PTY_DEBUG_LOGGING
          klog_puts("[PTY] ISIG: sending SIGQUIT to pgid=");
          klog_uint64(pgid);
          klog_puts("\n");
#endif
          extern void signal_send_pgid(uint32_t pgid, int sig);
          signal_send_pgid(pgid, 3); // SIGQUIT
          return (uint32_t)(i + 1);
        }
      }

      // Support for canonical mode backspace (VERASE)
      if ((pty->termios.c_lflag & ICANON) && c == pty->termios.c_cc[2]) {
        if (!ring_empty(pty->m2s_head, pty->m2s_tail)) {
          uint32_t prev =
              (pty->m2s_head + PTY_BUFFER_SIZE - 1) % PTY_BUFFER_SIZE;
          if (pty->master_to_slave[prev] != '\n') {
            pty->m2s_head = prev;
            written++;
            if (pty->termios.c_lflag & ECHO) {
              uint8_t bs[] = "\b \b";
              ring_write(pty->slave_to_master, &pty->s2m_head, pty->s2m_tail,
                         bs, 3);
              wake_master = true;
            }
          }
        }
        continue;
      }

      uint32_t n = ring_write(pty->master_to_slave, &pty->m2s_head,
                              pty->m2s_tail, &c, 1);
      if (n > 0) {
        written++;
        wake_slave = true;

        if (c == '\n') {
          pty->m2s_newline_count++;
        }

        if (pty->termios.c_lflag & ECHO) {
          if (c == '\n' && (pty->termios.c_oflag & ONLCR)) {
            uint8_t cr = '\r';
            ring_write(pty->slave_to_master, &pty->s2m_head, pty->s2m_tail, &cr,
                       1);
          }
#if PTY_DEBUG_LOGGING
          klog_puts("[PTY] ptmx_write: echoing char 0x");
          klog_hex64(c);
          klog_puts(" to s2m buffer\n");
#endif
          ring_write(pty->slave_to_master, &pty->s2m_head, pty->s2m_tail, &c,
                     1);
          wake_master = true;
        }
      } else {
        break;
      }
    }
    spinlock_release(&pty->lock);
  }

  if (wake_slave && pty->slave_waitq &&
      ((wait_queue_t *)pty->slave_waitq)->head != NULL) {
#if PTY_DEBUG_LOGGING
    klog_puts("[PTY] ptmx_write: waking slave waitq\n");
#endif
    wait_queue_wake_all((wait_queue_t *)pty->slave_waitq);
  }
  if (wake_master && pty->master_waitq &&
      ((wait_queue_t *)pty->master_waitq)->head != NULL) {
#if PTY_DEBUG_LOGGING
    klog_puts("[PTY] ptmx_write: waking master waitq\n");
#endif
    wait_queue_wake_all((wait_queue_t *)pty->master_waitq);
  }

#if PTY_DEBUG_LOGGING
  klog_puts("[PTY] ptmx_write: written=");
  klog_uint64(written);
  klog_puts(" m2s_newline_count=");
  klog_uint64(pty->m2s_newline_count);
  klog_puts("\n");
#endif

  return written;
}

/* TIOCINQ/FIONREAD: how many bytes a read() would return.  kpty asks this on
 * every read notification and skips the read entirely if the ioctl fails, which
 * is how a Konsole tab ends up showing nothing while bash is alive and running
 * commands.  A canonical slave only ever yields whole lines, and packet mode
 * costs the master one status byte per read.  Caller holds pty->lock. */
static uint32_t pty_bytes_pending(pty_pair_t *pty, bool to_master) {
  uint32_t used = to_master ? ring_used(pty->s2m_head, pty->s2m_tail)
                            : ring_used(pty->m2s_head, pty->m2s_tail);

  if (!to_master && (pty->termios.c_lflag & ICANON)) {
    if (pty->m2s_newline_count == 0)
      return 0;
    for (uint32_t i = 0; i < used; i++) {
      uint32_t at = (pty->m2s_tail + i) % PTY_BUFFER_SIZE;
      if (pty->master_to_slave[at] == '\n') {
        used = i + 1;
        break;
      }
    }
  } else if (to_master && pty->packet_mode) {
    used += 1; // the packet header read() prepends
  }
  return used;
}

int ptmx_ioctl(struct vfs_node *node, uint32_t request, uint64_t arg) {
  pty_pair_t *pty = (pty_pair_t *)node->device;
  if (!pty)
    return -9; // EBADF

  uint32_t pgid_to_signal = 0;
  int signal_to_send = 0;
  int ret = -25; // Default: ENOTTY

  spinlock_acquire(&pty->lock);

  switch (request) {
  case TCGETS: {
    struct kernel_termios *kt = (struct kernel_termios *)arg;
    if (!kt || !vmm_is_user_addr_range_valid(arg, sizeof(*kt))) {
      ret = -14;
    } else {
      kt->c_iflag = pty->termios.c_iflag;
      kt->c_oflag = pty->termios.c_oflag;
      kt->c_cflag = pty->termios.c_cflag;
      kt->c_lflag = pty->termios.c_lflag;
      kt->c_line = pty->termios.c_line;
      memcpy(kt->c_cc, pty->termios.c_cc, KERNEL_NCCS);
      ret = 0;
    }
    break;
  }

  case TCSETS:
  case TCSETSW:
  case TCSETSF: {
    const struct kernel_termios *kt = (const struct kernel_termios *)arg;
    if (!kt || !vmm_is_user_addr_range_valid(arg, sizeof(*kt))) {
      ret = -14;
    } else {
      pty->termios.c_iflag = kt->c_iflag;
      pty->termios.c_oflag = kt->c_oflag;
      pty->termios.c_cflag = kt->c_cflag;
      pty->termios.c_lflag = kt->c_lflag;
      pty->termios.c_line = kt->c_line;
      memcpy(pty->termios.c_cc, kt->c_cc, KERNEL_NCCS);
      ret = 0;
    }
    break;
  }

  case TIOCPKT: {
    const int *enabled = (const int *)arg;
    if (!enabled || !vmm_is_user_addr_range_valid(arg, sizeof(*enabled))) {
      ret = -14;
    } else {
      pty->packet_mode = (*enabled != 0);
      ret = 0;
    }
    break;
  }

  case TIOCINQ: /* same request number as FIONREAD */
  {
    int *pending = (int *)arg;
    if (!pending || !vmm_is_user_addr_range_valid(arg, sizeof(int))) {
      ret = -14; // EFAULT
    } else {
      *pending = (int)pty_bytes_pending(pty, true);
      ret = 0;
    }
    break;
  }

  case TIOCGPTN: {
    // Get PTY number
    int *ptn = (int *)arg;
    if (!ptn || !vmm_is_user_addr_range_valid(arg, sizeof(int))) {
      ret = -14; // EFAULT
    } else {
      *ptn = pty->index;
      ret = 0;
    }
    break;
  }

  case TIOCSPTLCK: {
    // Lock/unlock PTY
    int *lock = (int *)arg;
    if (!lock || !vmm_is_user_addr_range_valid(arg, sizeof(int))) {
      ret = -14;
    } else {
      pty->locked = (*lock != 0);
      ret = 0;
    }
    break;
  }

  case TIOCGPTLCK: {
    // Get lock state
    int *lock = (int *)arg;
    if (!lock || !vmm_is_user_addr_range_valid(arg, sizeof(int))) {
      ret = -14;
    } else {
      *lock = pty->locked ? 1 : 0;
      ret = 0;
    }
    break;
  }

  case TIOCSIG: {
    // Send signal to slave's foreground process group
    int sig = (int)arg;
    if (sig <= 0 || sig > 64) {
      ret = -22; // EINVAL
    } else {
      if (pty->pgid != 0) {
        pgid_to_signal = pty->pgid;
        signal_to_send = sig;
      }
      ret = 0;
    }
    break;
  }

  case TIOCGWINSZ: {
    struct winsize *ws = (struct winsize *)arg;
    if (!ws || !vmm_is_user_addr_range_valid(arg, sizeof(struct winsize))) {
      ret = -14;
    } else {
      *ws = pty->winsize;
      ret = 0;
    }
    break;
  }

  case TIOCSWINSZ: {
    const struct winsize *ws = (const struct winsize *)arg;
    if (!ws || !vmm_is_user_addr_range_valid(arg, sizeof(struct winsize))) {
      ret = -14;
    } else {
      pty->winsize = *ws;
      if (pty->pgid != 0) {
        pgid_to_signal = pty->pgid;
        signal_to_send = 28; // SIGWINCH
      }
      ret = 0;
    }
    break;
  }

  default:
    ret = -25; // ENOTTY
    break;
  }

  spinlock_release(&pty->lock);

  // Deliver signals outside the device lock
  if (pgid_to_signal != 0) {
    extern void signal_send_pgid(uint32_t pgid, int sig);
    signal_send_pgid(pgid_to_signal, signal_to_send);
  }

  return ret;
}

int ptmx_poll(struct vfs_node *node, int events) {
  pty_pair_t *pty = (pty_pair_t *)node->device;
  if (!pty)
    return 0;

  spinlock_acquire(&pty->lock);
  int revents = 0;

  if (events & (POLLIN | POLLRDNORM)) {
    // Master can read if slave has written anything
    if (!ring_empty(pty->s2m_head, pty->s2m_tail)) {
      revents |= (events & (POLLIN | POLLRDNORM));
    } else if (pty->slave_open_count == 0) {
      revents |= POLLHUP;
    }
  }

  if (events & (POLLOUT | POLLWRNORM)) {
    // Master is ready to write if master->slave buffer is not full
    if (!ring_full(pty->m2s_head, pty->m2s_tail)) {
      revents |= (events & (POLLOUT | POLLWRNORM));
    }
  }

  spinlock_release(&pty->lock);
  return revents;
}

void ptmx_close(struct vfs_node *node) {
  pty_pair_t *pty = (pty_pair_t *)node->device;
  if (!pty)
    return;

  spinlock_acquire(&pty->lock);
  pty->master_node = NULL;

  // Only mark master as closed if no slaves are open
  // This prevents breaking the slave when the terminal emulator
  // accidentally closes the master fd (e.g., by reusing fd for other files)
  if (pty->slave_open_count == 0) {
    pty->master_open = false;
    pty->allocated = false;
#if PTY_DEBUG_LOGGING
    klog_puts("[PTY] Deallocated PTY pair ");
    klog_uint64((uint64_t)pty->index);
    klog_puts(" (master closed, no slaves)\n");
#endif
  } else {
    // Keep master_open = true so slave can still write
    // The buffer will accumulate data until master is reopened
#if PTY_DEBUG_LOGGING
    klog_puts(
        "[PTY] Master fd closed but slaves still open, keeping PTY alive\n");
#endif
  }

  // Wake up anything waiting on the slave side to notify them of hangup
  if (pty->slave_waitq) {
    wait_queue_wake_all((wait_queue_t *)pty->slave_waitq);
  }

  spinlock_release(&pty->lock);
}

// /dev/pts/N (slave) VFS operations

uint32_t pty_slave_read(struct vfs_node *node, uint32_t offset, uint32_t size,
                        uint8_t *buffer) {
  (void)offset;

  pty_pair_t *pty = (pty_pair_t *)node->device;
  if (!pty)
    return 0;

#if PTY_DEBUG_LOGGING
  klog_puts("[PTY] slave_read: size=");
  klog_uint64(size);
  klog_puts("\n");
#endif

  spinlock_acquire(&pty->lock);
  // Slave reads from master→slave buffer
  while (1) {
    bool can_read = false;
    bool empty = ring_empty(pty->m2s_head, pty->m2s_tail);
#if PTY_DEBUG_LOGGING
    klog_puts("[PTY] slave_read: ring_empty=");
    klog_uint64(empty ? 1 : 0);
    klog_puts(" m2s_newline_count=");
    klog_uint64(pty->m2s_newline_count);
    klog_puts(" ICANON=");
    klog_uint64(pty->termios.c_lflag & ICANON ? 1 : 0);
    klog_puts("\n");
#endif

    if (!empty) {
      if (pty->termios.c_lflag & ICANON) {
        // In canonical mode, only allow reading if there's a newline
        if (pty->m2s_newline_count > 0) {
          can_read = true;
        }
      } else {
        can_read = true;
      }
    }

    if (can_read)
      break;

    if (!pty->master_open) {
      spinlock_release(&pty->lock);
#if PTY_DEBUG_LOGGING
      klog_puts("[PTY] slave_read: master closed, EOF\n");
#endif
      return 0; // EOF: Master closed
    }

    struct thread *current = sched_get_current();
    bool is_nonblock = (node->flags & FS_NONBLOCK) != 0;
    if (!is_nonblock && current) {
      for (int fd = 0; fd < MAX_FDS; fd++) {
        if (current->fds[fd] == node && (current->fd_flags[fd] & 0x800)) {
          is_nonblock = true;
          break;
        }
      }
    }
    if (is_nonblock) {
      spinlock_release(&pty->lock);
      return (uint32_t)-11; /* EAGAIN */
    }

    // Wait for data
#if PTY_DEBUG_LOGGING
    klog_puts("[PTY] slave_read: blocking on waitq\n");
#endif
    wait_queue_entry_t entry = {0};
    entry.thread = sched_get_current();
    wait_queue_add((wait_queue_t *)pty->slave_waitq, &entry);
    entry.thread->state = THREAD_BLOCKED;

    // Check availability again while BLOCKED
    bool recheck_can_read = false;
    if (!ring_empty(pty->m2s_head, pty->m2s_tail)) {
      if (!(pty->termios.c_lflag & ICANON) || pty->m2s_newline_count > 0) {
        recheck_can_read = true;
      }
    }

    if (recheck_can_read || !pty->master_open) {
      entry.thread->state = THREAD_RUNNING;
      spinlock_release(&pty->lock);
    } else {
      spinlock_release(&pty->lock);
      sched_yield();
    }

    spinlock_acquire(&pty->lock);
    wait_queue_remove((wait_queue_t *)pty->slave_waitq, &entry);
    entry.thread->state = THREAD_RUNNING;
#if PTY_DEBUG_LOGGING
    klog_puts("[PTY] slave_read: woke up\n");
#endif
  }

  uint32_t read = 0;
  if (pty->termios.c_lflag & ICANON) {
    // Read up to newline or size
    while (read < size && !ring_empty(pty->m2s_head, pty->m2s_tail)) {
      uint8_t c = pty->master_to_slave[pty->m2s_tail];
      buffer[read++] = c;
      pty->m2s_tail = (pty->m2s_tail + 1) % PTY_BUFFER_SIZE;
      if (c == '\n') {
        pty->m2s_newline_count--;
        break;
      }
    }
  } else {
    read = ring_read(pty->master_to_slave, pty->m2s_head, &pty->m2s_tail,
                     buffer, size);
  }

  spinlock_release(&pty->lock);

  if (read > 0) {
    /*
        klog_puts("[PTY] Slave read count=");
        klog_uint64((uint64_t)read);
        klog_puts("\n");
    */
  }

  return read;
}

uint32_t pty_slave_write(struct vfs_node *node, uint32_t offset, uint32_t size,
                         uint8_t *buffer) {
  (void)offset;

  pty_pair_t *pty = (pty_pair_t *)node->device;
  if (!pty) {
#if PTY_DEBUG_LOGGING
    klog_puts("[PTY] slave_write: no pty\n");
#endif
    return 0;
  }

#if PTY_DEBUG_LOGGING
  klog_puts("[PTY] slave_write: size=");
  klog_uint64(size);
  klog_puts("\n");
#endif

  uint32_t total_written = 0;

  // Check if output processing is needed
  bool do_opost = (pty->termios.c_oflag & OPOST) != 0;
  bool do_onlcr = (pty->termios.c_oflag & ONLCR) != 0;

  while (total_written < size) {
    spinlock_acquire(&pty->lock);

    if (!pty->master_open) {
#if PTY_DEBUG_LOGGING
      klog_puts("[PTY] slave_write: master not open\n");
#endif
      spinlock_release(&pty->lock);
      break; // Master gone - return what we've written so far
    }

    // Check if there's space in the buffer
    uint32_t free_space = ring_free(pty->s2m_head, pty->s2m_tail);

    if (free_space == 0) {
      struct thread *current = sched_get_current();
      bool is_nonblock = (node->flags & FS_NONBLOCK) != 0;
      if (!is_nonblock && current) {
        for (int fd = 0; fd < MAX_FDS; fd++) {
          if (current->fds[fd] == node && (current->fd_flags[fd] & 0x800)) {
            is_nonblock = true;
            break;
          }
        }
      }
      if (is_nonblock) {
        spinlock_release(&pty->lock);
        return total_written > 0 ? total_written : (uint32_t)-11; /* EAGAIN */
      }

      // Buffer full - block until master reads
      // Wake up master in case it's waiting
      if (pty->master_waitq &&
          ((wait_queue_t *)pty->master_waitq)->head != NULL) {
        wait_queue_wake_all(pty->master_waitq);
      }

      // Wait for space
      wait_queue_entry_t entry = {0};
      entry.thread = sched_get_current();
      wait_queue_add((wait_queue_t *)pty->slave_write_waitq, &entry);
      entry.thread->state = THREAD_BLOCKED;

      // Re-check space while BLOCKED
      if (ring_free(pty->s2m_head, pty->s2m_tail) > 0 || !pty->master_open) {
        entry.thread->state = THREAD_RUNNING;
        spinlock_release(&pty->lock);
      } else {
        spinlock_release(&pty->lock);
        sched_yield();
      }

      spinlock_acquire(&pty->lock);
      wait_queue_remove((wait_queue_t *)pty->slave_write_waitq, &entry);
      entry.thread->state = THREAD_RUNNING;

      // Re-check master status after waking
      if (!pty->master_open) {
        spinlock_release(&pty->lock);
        break;
      }

      free_space = ring_free(pty->s2m_head, pty->s2m_tail);
      if (free_space == 0) {
        // Still no space - try again
        spinlock_release(&pty->lock);
        continue;
      }
    }

    if (do_opost && do_onlcr) {
      // Output processing: convert \n to \r\n
      while (total_written < size) {
        free_space = ring_free(pty->s2m_head, pty->s2m_tail);
        if (free_space == 0)
          break;

        uint8_t c = buffer[total_written];
        if (c == '\n') {
          // Need at least 2 bytes of space for \r\n
          if (free_space < 2)
            break;
          uint8_t crlf[2] = {'\r', '\n'};
          ring_write(pty->slave_to_master, &pty->s2m_head, pty->s2m_tail, crlf,
                     2);
        } else {
          ring_write(pty->slave_to_master, &pty->s2m_head, pty->s2m_tail, &c,
                     1);
        }
        total_written++;
      }
    } else {
      // No output processing - fast path
      uint32_t to_write = size - total_written;
      if (to_write > free_space)
        to_write = free_space;
      uint32_t written_now =
          ring_write(pty->slave_to_master, &pty->s2m_head, pty->s2m_tail,
                     buffer + total_written, to_write);
      total_written += written_now;
    }

    spinlock_release(&pty->lock);

    // Wake ordinary readers and epoll watchers if we wrote something.
    if (total_written > 0 && pty->master_node)
      epoll_notify_event(pty->master_node, EPOLLIN);
    if (pty->master_waitq &&
        ((wait_queue_t *)pty->master_waitq)->head != NULL) {
      wait_queue_wake_all(pty->master_waitq);
    }
  }

#if PTY_DEBUG_LOGGING
  klog_puts("[PTY] slave_write: total_written=");
  klog_uint64(total_written);
  klog_puts("\n");
#endif
  return total_written;
}

// Rescan buffer for newlines (used when switching to canonical mode)
static void pty_rescan_newlines(pty_pair_t *pty) {
  pty->m2s_newline_count = 0;
  uint32_t curr = pty->m2s_tail;
  while (curr != pty->m2s_head) {
    if (pty->master_to_slave[curr] == '\n') {
      pty->m2s_newline_count++;
    }
    curr = (curr + 1) % PTY_BUFFER_SIZE;
  }
}

int pty_slave_ioctl(struct vfs_node *node, uint32_t request, uint64_t arg) {
  pty_pair_t *pty = (pty_pair_t *)node->device;
  if (!pty)
    return -9; // EBADF

  uint32_t pgid_to_signal = 0;
  int signal_to_send = 0;
  int ret = -25; // Default: ENOTTY

  spinlock_acquire(&pty->lock);

  switch (request) {
  case TCGETS: {
    if (!arg || !vmm_is_user_addr_range_valid(arg, sizeof(struct kernel_termios))) {
      ret = -14;
    } else {
      struct kernel_termios kt;
      kt.c_iflag = pty->termios.c_iflag;
      kt.c_oflag = pty->termios.c_oflag;
      kt.c_cflag = pty->termios.c_cflag;
      kt.c_lflag = pty->termios.c_lflag;
      kt.c_line  = pty->termios.c_line;
      memcpy(kt.c_cc, pty->termios.c_cc, KERNEL_NCCS);
      memcpy((void *)arg, &kt, sizeof(struct kernel_termios));
      ret = 0;
    }
    break;
  }

  case TCSETS:
  case TCSETSW:
  case TCSETSF: {
    if (!arg || !vmm_is_user_addr_range_valid(arg, sizeof(struct kernel_termios))) {
      ret = -14;
    } else {
      struct kernel_termios kt;
      memcpy(&kt, (const void *)arg, sizeof(struct kernel_termios));
      bool old_icanon = (pty->termios.c_lflag & ICANON);
#if PTY_DEBUG_LOGGING
      klog_puts("[PTY] slave_ioctl TCSETS: old ICANON=");
      klog_uint64(old_icanon ? 1 : 0);
      klog_puts(" new ICANON=");
      klog_uint64(kt.c_lflag & ICANON ? 1 : 0);
      klog_puts(" new ECHO=");
      klog_uint64(kt.c_lflag & ECHO ? 1 : 0);
      klog_puts("\n");
#endif
      pty->termios.c_iflag = kt.c_iflag;
      pty->termios.c_oflag = kt.c_oflag;
      pty->termios.c_cflag = kt.c_cflag;
      pty->termios.c_lflag = kt.c_lflag;
      pty->termios.c_line  = kt.c_line;
      memcpy(pty->termios.c_cc, kt.c_cc, KERNEL_NCCS);
      if (!old_icanon && (pty->termios.c_lflag & ICANON)) {
        pty_rescan_newlines(pty);
      }
      ret = 0;
    }
    break;
  }

  case TIOCGWINSZ: {
    struct winsize *ws = (struct winsize *)arg;
    if (!ws || !vmm_is_user_addr_range_valid(arg, sizeof(struct winsize))) {
      ret = -14;
    } else {
      *ws = pty->winsize;
      ret = 0;
    }
    break;
  }

  case TIOCSWINSZ: {
    const struct winsize *ws = (const struct winsize *)arg;
    if (!ws || !vmm_is_user_addr_range_valid(arg, sizeof(struct winsize))) {
      ret = -14;
    } else {
      pty->winsize = *ws;
      if (pty->pgid != 0) {
        pgid_to_signal = pty->pgid;
        signal_to_send = 28; // SIGWINCH
      }
      ret = 0;
    }
    break;
  }

  case TIOCINQ: /* same request number as FIONREAD */
  {
    int *pending = (int *)arg;
    if (!pending || !vmm_is_user_addr_range_valid(arg, sizeof(int))) {
      ret = -14; // EFAULT
    } else {
      *pending = (int)pty_bytes_pending(pty, false);
      ret = 0;
    }
    break;
  }

  case TIOCGPGRP: {
    int *pgrp = (int *)arg;
    if (!pgrp || !vmm_is_user_addr_range_valid(arg, sizeof(int))) {
      ret = -14;
    } else {
      *pgrp = (int)pty->pgid;
      ret = 0;
    }
    break;
  }

  case TIOCSPGRP: {
    // Set foreground process group
    int *pgrp = (int *)arg;
    if (!pgrp || !vmm_is_user_addr_range_valid(arg, sizeof(int))) {
      ret = -14;
    } else {
      pty->pgid = (uint32_t)*pgrp;
      ret = 0;
    }
    break;
  }

  case 0x540E: { // TIOCSCTTY — make this PTY the controlling terminal
    struct thread *t = sched_get_current();
    if (t) {
      t->ctty = node;
      // Also set the PTY's foreground pgid to the caller's pgid if not set
      if (pty->pgid == 0)
        pty->pgid = t->pgid;
#if PTY_DEBUG_LOGGING
      klog_puts("[PTY] TIOCSCTTY: set ctty for tid=");
      klog_uint64(t->tid);
      klog_puts("\n");
#endif
    }
    ret = 0;
    break;
  }

  default:
    ret = -25; // ENOTTY
    break;
  }

  spinlock_release(&pty->lock);

  if (pgid_to_signal != 0) {
    extern void signal_send_pgid(uint32_t pgid, int sig);
    signal_send_pgid(pgid_to_signal, signal_to_send);
  }

  return ret;
}

int pty_slave_poll(struct vfs_node *node, int events) {
  pty_pair_t *pty = (pty_pair_t *)node->device;
  if (!pty)
    return 0;

  spinlock_acquire(&pty->lock);
  int revents = 0;

  if (events & (POLLIN | POLLRDNORM)) {
    // Slave can read if master has written anything
    if (!ring_empty(pty->m2s_head, pty->m2s_tail)) {
      revents |= (events & (POLLIN | POLLRDNORM));
    } else if (!pty->master_open) {
      revents |= POLLHUP;
    }
  }

  if (events & (POLLOUT | POLLWRNORM)) {
    // Slave is ready to write if slave->master buffer is not full
    if (!ring_full(pty->s2m_head, pty->s2m_tail)) {
      revents |= (events & (POLLOUT | POLLWRNORM));
    }
  }

  spinlock_release(&pty->lock);
  return revents;
}

void pty_slave_open(struct vfs_node *node) {
  pty_pair_t *pty = (pty_pair_t *)node->device;
  if (!pty)
    return;

  spinlock_acquire(&pty->lock);
  // vfs_open() is also used for dup() and inherited descriptors, while the
  // VFS close hook runs only when this node's final reference is released.
  // Remember how many slave references this particular node contributed so
  // the final close can remove all of them from the pair-wide count.
  node->impl++;
  pty->slave_open_count++;
  spinlock_release(&pty->lock);

  // Set this as the controlling terminal for the current process
  struct thread *t = sched_get_current();
  if (t && !t->ctty) {
    t->ctty = node;
#if PTY_DEBUG_LOGGING
    klog_puts("[PTY] Set ctty for tid=");
    klog_uint64(t->tid);
    klog_puts("\n");
#endif
  }
}

void pty_slave_close(struct vfs_node *node) {
  pty_pair_t *pty = (pty_pair_t *)node->device;
  if (!pty)
    return;

  spinlock_acquire(&pty->lock);
  int open_refs = (int)node->impl;
  if (open_refs > pty->slave_open_count)
    open_refs = pty->slave_open_count;
  pty->slave_open_count -= open_refs;
  node->impl = 0;

  // Wake up anything waiting on the master side to notify them of hangup
  if (pty->master_waitq) {
    wait_queue_wake_all((wait_queue_t *)pty->master_waitq);
  }

  // If master is already closed and this was the last slave, deallocate
  if (!pty->master_open && pty->slave_open_count == 0) {
    pty->allocated = false;
#if PTY_DEBUG_LOGGING
    klog_puts("[PTY] Deallocated PTY pair ");
    klog_uint64((uint64_t)pty->index);
    klog_puts(" (last slave closed)\n");
#endif
  }

  spinlock_release(&pty->lock);
}

uint64_t ptmx_mmap(struct vfs_node *node, uint64_t addr, uint64_t length,
                   uint64_t prot, uint64_t flags, uint64_t offset) {
  (void)offset;

  pty_pair_t *pty = (pty_pair_t *)node->device;
  if (!pty)
    return (uint64_t)-1;

  if (length == 0 || length > 4096)
    return (uint64_t)-1;

  if (!pty->shared_mmap_frame) {
    pty->shared_mmap_frame = pmm_alloc();
    if (!pty->shared_mmap_frame)
      return (uint64_t)-1;
    memset((void *)((uint64_t)pty->shared_mmap_frame + pmm_get_hhdm_offset()),
           0, 4096);
  }

  uint64_t vaddr = addr;
  if (!(flags & 0x10) || vaddr == 0) { // MAP_FIXED
    vaddr = mm_alloc_mmap_region(4096);
  }

  if (vaddr == 0)
    return (uint64_t)-1;

  uint64_t page_flags = PAGE_FLAG_PRESENT | PAGE_FLAG_USER;
  if (prot & 0x02) // PROT_WRITE
    page_flags |= PAGE_FLAG_RW;
  if (!(prot & 0x04)) // PROT_EXEC
    page_flags |= PAGE_FLAG_NX;

  if (!vmm_map_page(vmm_get_active_pml4(), vaddr,
                    (uint64_t)pty->shared_mmap_frame, page_flags)) {
    return (uint64_t)-1;
  }

  return vaddr;
}

uint64_t pty_slave_mmap(struct vfs_node *node, uint64_t addr, uint64_t length,
                        uint64_t prot, uint64_t flags, uint64_t offset) {
  // Master and slave share the same mmap frame logic
  return ptmx_mmap(node, addr, length, prot, flags, offset);
}

// Device registration

void pty_register_devices(void) {
  pty_init();

  // /dev/ptmx - PTY master multiplexor
  // This is a special device: each open creates a new PTY pair
  // The device pointer will be set dynamically on open
  vfs_node_t *ptmx_node = kmalloc(sizeof(vfs_node_t));
  if (!ptmx_node)
    return;

  vfs_node_init(ptmx_node);
#if PTY_DEBUG_LOGGING
  klog_puts("[PTY] pty_slave_write addr=0x");
  klog_hex64((uint64_t)pty_slave_write);
  klog_puts("\n");
#endif
  strcpy(ptmx_node->name, "ptmx");
  ptmx_node->flags = FS_CHARDEV;
  ptmx_node->mask = 0666;
  ptmx_node->read = ptmx_read;
  ptmx_node->write = ptmx_write;
  ptmx_node->ioctl = ptmx_ioctl;
  ptmx_node->poll = ptmx_poll;
  ptmx_node->device = NULL; // Set on open

  fb_register_device_node("ptmx", ptmx_node);

#if PTY_DEBUG_LOGGING
  klog_puts("[PTY] Registered /dev/ptmx\n");
#endif
}
