#ifndef DRIVERS_PTY_H
#define DRIVERS_PTY_H

#include "../fb/terminal.h"
#include "../lock/spinlock.h"
#include <stdbool.h>
#include <stdint.h>

#ifndef PTY_DEBUG_LOGGING
#define PTY_DEBUG_LOGGING 0
#endif

#define PTY_MAX_PAIRS 16
#define PTY_BUFFER_SIZE 4096

// Terminal ioctl commands (Linux x86_64 compatible)
#define TCGETS 0x5401
#define TCSETS 0x5402
#define TCSETSW 0x5403
#define TCSETSF 0x5404

#define TIOCGWINSZ 0x5413
#define TIOCSWINSZ 0x5414

#define TIOCGPGRP 0x540F
#define TIOCSPGRP 0x5410
/* Bytes waiting to be read.  TIOCINQ and FIONREAD are the same request on Linux
 * (0x541B).  kpty asks this before every read of the master and skips the read
 * outright when the ioctl fails, so without it a terminal shows nothing at all
 * while the shell behind it keeps running. */
#define TIOCINQ 0x541B
#define FIONREAD 0x541B

// PTY ioctl commands (Linux compatible)
#define TIOCGPTN 0x80045430   // Get PTY number
#define TIOCSPTLCK 0x40045431 // Lock/unlock PTY
#define TIOCGPTLCK 0x80045432 // Get PTY lock state
#define TIOCSIG 0x40045436    // Send signal to slave
#define TIOCPKT 0x5420        // Enable/disable PTY packet mode

// Forward declaration
struct vfs_node;

typedef struct pty_pair {
  int index;            // PTY index (0-15)
  bool allocated;       // Is this pair in use?
  bool locked;          // Is slave locked (cannot be opened)?
  bool master_open;     // Is the master side open?
  bool packet_mode;     // Prefix master reads with a TIOCPKT control byte
  struct vfs_node *master_node; // Dynamic node watched by poll/epoll
  int slave_open_count; // Number of open slave handles

  // Master → Slave buffer (data written by master, read by slave)
  uint8_t master_to_slave[PTY_BUFFER_SIZE];
  uint32_t m2s_head;          // Write position (master writes here)
  uint32_t m2s_tail;          // Read position (slave reads here)
  uint32_t m2s_newline_count; // Number of newlines in m2s buffer

  // Slave → Master buffer (data written by slave, read by master)
  uint8_t slave_to_master[PTY_BUFFER_SIZE];
  uint32_t s2m_head; // Write position (slave writes here)
  uint32_t s2m_tail; // Read position (master reads here)

  // Terminal settings for slave
  struct termios termios;
  struct winsize winsize;

  // Foreground process group for signals
  uint32_t pgid;

  // Wait queues for blocking I/O
  void *master_waitq;      // Readers waiting on master (slave→master data)
  void *slave_waitq;       // Readers waiting on slave (master→slave data)
  void *slave_write_waitq; // Writers waiting for space in slave→master buffer
  void *shared_mmap_frame; // Shared frame for mmap
  spinlock_t lock;         // Lock for thread-safe access
} pty_pair_t;

// Initialize PTY subsystem
void pty_init(void);

// Create a new PTY pair (called when opening /dev/ptmx)
// Returns the PTY index, or -1 on error
int pty_alloc_pair(void);

// Get PTY pair by index
pty_pair_t *pty_get_pair(int index);

// VFS operations for master device (/dev/ptmx)
uint32_t ptmx_read(struct vfs_node *node, uint32_t offset, uint32_t size,
                   uint8_t *buffer);
uint32_t ptmx_write(struct vfs_node *node, uint32_t offset, uint32_t size,
                    uint8_t *buffer);
int ptmx_ioctl(struct vfs_node *node, uint32_t request, uint64_t arg);
int ptmx_poll(struct vfs_node *node, int events);
void ptmx_close(struct vfs_node *node);
uint64_t ptmx_mmap(struct vfs_node *node, uint64_t addr, uint64_t length,
                   uint64_t prot, uint64_t flags, uint64_t offset);

// VFS operations for slave device (/dev/pts/N)
uint32_t pty_slave_read(struct vfs_node *node, uint32_t offset, uint32_t size,
                        uint8_t *buffer);
uint32_t pty_slave_write(struct vfs_node *node, uint32_t offset, uint32_t size,
                         uint8_t *buffer);
int pty_slave_ioctl(struct vfs_node *node, uint32_t request, uint64_t arg);
int pty_slave_poll(struct vfs_node *node, int events);
void pty_slave_open(struct vfs_node *node);
void pty_slave_close(struct vfs_node *node);
uint64_t pty_slave_mmap(struct vfs_node *node, uint64_t addr, uint64_t length,
                        uint64_t prot, uint64_t flags, uint64_t offset);

// Check if data is available for reading
bool pty_master_can_read(pty_pair_t *pty);
bool pty_slave_can_read(pty_pair_t *pty);

// Register /dev/ptmx and /dev/pts directory
void pty_register_devices(void);

#endif
