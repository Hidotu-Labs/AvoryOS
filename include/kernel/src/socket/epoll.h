#ifndef EPOLL_H
#define EPOLL_H

#include "../lock/spinlock.h"
#include "../lib/list.h"
#include "../sched/wait.h"
#include <stdint.h>
#include <stdbool.h>

// epoll_event Structure (user-space visible)
typedef union epoll_data {
  void *ptr;
  int fd;
  uint32_t u32;
  uint64_t u64;
} epoll_data_t;

// Must match musl's struct layout on x86_64 (packed, 12 bytes)
// Musl uses __attribute__((__packed__)) on x86_64
struct epoll_event {
  uint32_t events;     // Epoll events (EPOLLIN, EPOLLOUT, etc.) - offset 0
  epoll_data_t data;   // User data variable - offset 4
} __attribute__((packed));

// Epoll Event Flags
#define EPOLLIN     0x00000001  // Available for read
#define EPOLLPRI    0x00000002  // Priority data available
#define EPOLLOUT    0x00000004  // Available for write
#define EPOLLRDNORM 0x00000040  // Normal data readable
#define EPOLLRDBAND 0x00000080  // Priority data readable
#define EPOLLWRNORM 0x00000100  // Normal data writable
#define EPOLLWRBAND 0x00000200  // Priority data writable
#define EPOLLMSG    0x00000400  // Message available
#define EPOLLERR    0x00000008  // Error condition
#define EPOLLHUP    0x00000010  // Hang up
#define EPOLLRDHUP  0x00002000  // Peer closed write side
#define EPOLLEXCLUSIVE 0x10000000U  // Exclusive wake-up (prevent thundering herd)
#define EPOLLWAKEUP 0x20000000U     // Wake on event
#define EPOLLONESHOT 0x40000000U    // One-shot mode (disable after event)
#define EPOLLET     0x80000000U     // Edge-triggered mode

// epoll_ctl Operations
#define EPOLL_CTL_ADD 1  // Add a file descriptor
#define EPOLL_CTL_DEL 2  // Remove a file descriptor
#define EPOLL_CTL_MOD 3  // Modify a file descriptor

// epoll_create1 Flags
#define EPOLL_CLOEXEC 0x80000  // O_CLOEXEC (1 << 19)

// Maximum epoll settings
#define EPOLL_MAX_INSTANCES  64    // Max epoll instances system-wide
#define EPOLL_MAX_WATCHED    4096  // Max FDs per epoll instance
#define EPOLL_MAX_EVENTS     128   // Max events returned per epoll_wait

// Forward Declarations
struct vfs_node;
struct eventpoll;
struct epitem;

// Epoll Item Structure (per-watched FD)
typedef struct epitem {
  struct list_head rdllink;    // Link to ready list
  struct list_head fllink;     // Link to fd list
  
  int fd;                      // File descriptor being watched
  struct vfs_node *node;       // VFS node for the FD
  struct list_head ep_node_link; // Link to vfs_node_t's epitem_list
  struct epoll_event event;    // Event mask and user data
  
  struct eventpoll *ep;        // Parent epoll instance
  
  // Edge-triggered state tracking
  uint32_t last_events;        // Last known event state (for ET mode)
  bool on_ready_list;          // Currently on ready list
  bool oneshot;                // One-shot mode active
  bool oneshot_disabled;        // One-shot has fired, needs re-arm
  bool exclusive;              // Exclusive wake-up mode
  
  // Store original events for oneshot re-arming
  uint32_t registered_events;  // Original event mask registered
  
  spinlock_t lock;
} epitem_t;

// Eventpoll Structure (per epoll instance)
typedef struct eventpoll {
  int fd;                      // FD of this epoll instance
  struct vfs_node *vfs_node;   // VFS node for this epoll fd (for nested epoll)

  // Watched FDs - simple array for O(1) lookup
  epitem_t *items[EPOLL_MAX_WATCHED];
  int item_count;
  /* Highest fd index ever used + 1.  Only grows while the instance is alive,
   * so the missed-wakeup rescan in epoll_wait_impl() can walk this instead of
   * all EPOLL_MAX_WATCHED slots on every pass. */
  int items_high;
  
  // Ready list - FDs with events pending
  struct list_head rdllist;    // Ready list head
  int rdllist_count;           // Number of ready items
  
  // Wait queue for blocking in epoll_wait
  wait_queue_t wq;
  
  // Lock for this structure
  spinlock_t lock;
  
  // Reference count
  uint64_t refcount;
} eventpoll_t;

// Epoll Subsystem Initialization
void epoll_init(void);

// Epoll Instance Management
eventpoll_t *epoll_create(void);
void epoll_destroy(eventpoll_t *ep);
void epoll_get(eventpoll_t *ep);
void epoll_put(eventpoll_t *ep);

// Epoll Operations
int epoll_ctl_add(eventpoll_t *ep, int fd, struct epoll_event *event);
int epoll_ctl_del(eventpoll_t *ep, int fd);
int epoll_ctl_mod(eventpoll_t *ep, int fd, struct epoll_event *event);

int epoll_wait_impl(eventpoll_t *ep, struct epoll_event *events, 
                    int maxevents, int timeout_ms);

// Epoll FD Management
int epoll_alloc_fd(eventpoll_t *ep);
eventpoll_t *epoll_from_fd(int fd);
int epoll_close_fd(int fd);

// VFS Integration
uint32_t epoll_vfs_read(struct vfs_node *node, uint32_t offset, 
                        uint32_t size, uint8_t *buffer);
uint32_t epoll_vfs_write(struct vfs_node *node, uint32_t offset, 
                         uint32_t size, uint8_t *buffer);
void epoll_vfs_open(struct vfs_node *node);
void epoll_vfs_close(struct vfs_node *node);
int epoll_vfs_poll(struct vfs_node *node, int events);

// Event Notification (called from socket/file subsystems)
void epoll_notify_event(struct vfs_node *node, uint32_t events);

// Notify epoll by socket FD (for abstract sockets without VFS node)
void epoll_notify_socket(int fd, uint32_t events);



// Check if oneshot needs re-arming
bool epoll_oneshot_is_disabled(epitem_t *epi);

// Re-arm oneshot after event delivery
void epoll_oneshot_rearm(epitem_t *epi);

// Exclusive wake-up: wake only one epoll instance
void epoll_notify_exclusive(struct vfs_node *node, uint32_t events);

#endif // EPOLL_H
