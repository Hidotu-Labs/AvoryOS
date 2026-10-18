#include "../sched/net_namespace.h"
#include "../sched/capability.h"
// Socket Subsystem Core Implementation

// socket creation and FD management

#include "socket.h"
#include "../console/klog.h"
#include "../fs/vfs.h"
#include "../lib/string.h"
#include "../mm/heap.h"
#include "../mm/vmm.h"
#include "../sched/sched.h"
#include "../sched/wait.h"
#include "af_unix.h"
#include "af_netlink.h"
#include "af_inet.h"
#include "af_inet6.h"
#include "af_qrtr.h"
#include "socket_internal.h"
#include "../syscalls/sys_io_shared.h"
#include "../net/core.h"
#include "../net/ipv4.h"
#include <stdint.h>

// Global Socket Table
socket_t *socket_table[SOCKET_MAX_COUNT];
int socket_count = 0;
spinlock_t socket_table_lock = SPINLOCK_INIT;

/* Linux x86_64 SIOCGIFCONF ABI: ifconf is 16 bytes and each IPv4 ifreq is
 * 40 bytes. Avory currently routes IPv4 through the default net_device. */
struct avory_ifconf64 {
  int32_t length;
  uint32_t _pad;
  uint64_t buffer;
};

struct avory_ifreq64 {
  char name[16];
  uint16_t family;
  uint8_t address_data[14];
  uint8_t _union_padding[8];
};
_Static_assert(sizeof(struct avory_ifconf64) == 16, "Linux x86_64 ifconf ABI");
_Static_assert(sizeof(struct avory_ifreq64) == 40, "Linux x86_64 ifreq ABI");

static int socket_ioctl_ifconf(uint64_t arg) {
  if (!arg || !vmm_is_user_addr_range_valid(arg, sizeof(struct avory_ifconf64)))
    return -14;

  struct avory_ifconf64 ifc;
  memcpy(&ifc, (const void *)arg, sizeof(ifc));
  if (ifc.length < 0)
    return -22;

  struct net_device *dev = net_device_default();
  const struct ipv4_config *cfg = ipv4_get_config();
  bool have_ipv4 = dev && dev->registered && cfg && cfg->address != 0;
  int written = 0;

  if (have_ipv4 && ifc.length >= (int32_t)sizeof(struct avory_ifreq64)) {
    if (!ifc.buffer || !vmm_is_user_addr_range_valid(
                           ifc.buffer, sizeof(struct avory_ifreq64)))
      return -14;

    struct avory_ifreq64 ifr;
    memset(&ifr, 0, sizeof(ifr));
    size_t name_len = strlen(dev->name);
    if (name_len >= sizeof(ifr.name))
      name_len = sizeof(ifr.name) - 1;
    memcpy(ifr.name, dev->name, name_len);
    ifr.family = 2; /* AF_INET */
    /* sockaddr_in address begins after family and the two-byte port. */
    ifr.address_data[2] = (uint8_t)(cfg->address >> 24);
    ifr.address_data[3] = (uint8_t)(cfg->address >> 16);
    ifr.address_data[4] = (uint8_t)(cfg->address >> 8);
    ifr.address_data[5] = (uint8_t)cfg->address;
    memcpy((void *)ifc.buffer, &ifr, sizeof(ifr));
    written = sizeof(ifr);
  }

  ifc.length = written;
  memcpy((void *)arg, &ifc, sizeof(ifc));
  return 0;
}

static bool socket_ifreq_name_is(const char requested[16], const char *actual) {
  if (!actual || !requested[0])
    return false;
  for (size_t i = 0; i < 16; i++) {
    if (requested[i] != actual[i])
      return false;
    if (!actual[i])
      return true;
  }
  return true;
}

static int socket_ioctl_ifreq(uint32_t request, uint64_t arg) {
  if (!arg || !vmm_is_user_addr_range_valid(arg, sizeof(struct avory_ifreq64)))
    return -14;

  struct avory_ifreq64 ifr;
  if (copy_from_user(&ifr, (const void *)arg, sizeof(ifr)) != 0)
    return -14;
  struct net_device *dev = net_device_default();
  bool loopback = socket_ifreq_name_is(ifr.name, "lo");

  if (request == 0x8910) { /* SIOCGIFNAME: input is an index, not a name. */
    int32_t ifindex;
    memcpy(&ifindex, &ifr.family, sizeof(ifindex));
    const char *name;
    if (ifindex == 1)
      name = "lo";
    else if (ifindex == 2 && dev && dev->registered)
      name = dev->name;
    else
      return -19; /* ENODEV */
    memset(ifr.name, 0, sizeof(ifr.name));
    strncpy(ifr.name, name, sizeof(ifr.name) - 1);
    return copy_to_user((void *)arg, &ifr, sizeof(ifr)) ? -14 : 0;
  }

  if (!loopback && (!dev || !dev->registered ||
                    !socket_ifreq_name_is(ifr.name, dev->name)))
    return -19; /* ENODEV */

  if (request == 0x8915) { /* SIOCGIFADDR */
    const struct ipv4_config *cfg = ipv4_get_config();
    uint32_t address = loopback ? 0x7f000001U : (cfg ? cfg->address : 0);
    if (!address)
      return -99; /* EADDRNOTAVAIL */
    memset(&ifr.family, 0, sizeof(ifr) - 16);
    ifr.family = 2; /* AF_INET */
    ifr.address_data[2] = (uint8_t)(address >> 24);
    ifr.address_data[3] = (uint8_t)(address >> 16);
    ifr.address_data[4] = (uint8_t)(address >> 8);
    ifr.address_data[5] = (uint8_t)address;
  } else if (request == 0x8933) { /* SIOCGIFINDEX */
    uint32_t ifindex = loopback ? 1 : 2; /* Match route netlink. */
    memset(&ifr.family, 0, sizeof(ifr) - 16);
    memcpy(&ifr.family, &ifindex, sizeof(ifindex));
  } else {
    return -25;
  }

  return copy_to_user((void *)arg, &ifr, sizeof(ifr)) ? -14 : 0;
}

// Socket Family Registry
static net_family_t *family_registry = NULL;

// Socket Table Management

int socket_table_alloc(void) {
  spinlock_acquire(&socket_table_lock);

  for (int i = 0; i < SOCKET_MAX_COUNT; i++) {
    if (socket_table[i] == NULL) {
      // Found empty slot
      socket_count++;
      spinlock_release(&socket_table_lock);
      return i;
    }
  }

  spinlock_release(&socket_table_lock);
  return -1; // Table full
}

void socket_table_free(int idx) {
  if (idx < 0 || idx >= SOCKET_MAX_COUNT)
    return;

  spinlock_acquire(&socket_table_lock);
  socket_table[idx] = NULL;
  socket_count--;
  spinlock_release(&socket_table_lock);
}

socket_t *socket_table_get(int idx) {
  if (idx < 0 || idx >= SOCKET_MAX_COUNT)
    return NULL;

  spinlock_acquire(&socket_table_lock);
  socket_t *sock = socket_table[idx];
  spinlock_release(&socket_table_lock);
  return sock;
}

// Socket Buffer Management

sk_buff_t *alloc_skb(size_t size) {
  sk_buff_t *skb = kmalloc(sizeof(sk_buff_t));
  if (!skb)
    return NULL;

  memset(skb, 0, sizeof(*skb));
  skb->data = kmalloc(size);
  if (!skb->data) {
    kfree(skb);
    return NULL;
  }

  skb->len = 0;
  skb->capacity = size;
  skb->next = NULL;
  return skb;
}

void free_skb(sk_buff_t *skb) {
  if (!skb)
    return;
  if (skb->data)
    kfree(skb->data);
  kfree(skb);
}

void skb_queue_tail(sk_buff_head_t *list, sk_buff_t *skb) {
  spinlock_acquire(&list->lock);

  skb->next = NULL;
  if (list->tail)
    list->tail->next = skb;
  else
    list->head = skb;
  list->tail = skb;
  list->len++;

  spinlock_release(&list->lock);
}

void skb_queue_head(sk_buff_head_t *list, sk_buff_t *skb) {
  spinlock_acquire(&list->lock);

  skb->next = list->head;
  list->head = skb;
  if (!list->tail)
    list->tail = skb;
  list->len++;

  spinlock_release(&list->lock);
}

sk_buff_t *skb_dequeue(sk_buff_head_t *list) {
  spinlock_acquire(&list->lock);

  if (!list->head) {
    spinlock_release(&list->lock);
    return NULL;
  }

  sk_buff_t *skb = list->head;
  list->head = skb->next;
  if (!list->head)
    list->tail = NULL;
  skb->next = NULL;
  list->len--;

  spinlock_release(&list->lock);
  return skb;
}

bool skb_queue_empty(sk_buff_head_t *list) { return list->head == NULL; }

// Socket Wait Queue Helpers

void socket_wait_queue_init(socket_t *sock) {
  _Static_assert(sizeof(wait_queue_t) <= sizeof(sock->_wait_queue_mem),
                 "wait_queue_t grew: enlarge socket_t._wait_queue_mem");
  wait_queue_init((wait_queue_t *)sock->_wait_queue_mem);
  sock->wait_queue = sock->_wait_queue_mem;
}

void socket_wait(socket_t *sock) {
  if (!sock->wait_queue)
    return;

  struct thread *current = sched_get_current();
  if (!current)
    return;

  /* NOTE: unconditional wait, no condition to recheck. Callers with a
   * condition must add+recheck under their object lock instead of using
   * this (see netlink_recvfrom). Kept race-narrow: BLOCKED before add. */
  wait_queue_entry_t entry = {.thread = current, .next = NULL, .wq = NULL, .thread_next = NULL, .thread_prev = NULL};

  current->state = THREAD_BLOCKED;
  wait_queue_add(sock->wait_queue, &entry);

  sched_yield();

  wait_queue_remove(sock->wait_queue, &entry);
  current->state = THREAD_RUNNING;
}

void socket_wake(socket_t *sock) {
  if (sock->wait_queue)
    wait_queue_wake_all(sock->wait_queue);
}

// Socket Creation/Destruction

socket_t *socket_create(int domain, int type, int protocol) {
  struct thread *owner = sched_get_current();
  return socket_create_in_namespace(domain, type, protocol, owner ? owner->net_ns : NULL);
}
socket_t *socket_create_in_namespace(int domain, int type, int protocol, struct net_namespace *net_ns) {
  if (net_ns && domain == AF_QIPCRTR) return NULL;
  // Validate domain
  if (domain != AF_UNIX && domain != AF_INET && domain != AF_INET6 &&
      domain != AF_NETLINK && domain != AF_QIPCRTR) {
    return NULL; // EAFNOSUPPORT
  }

  // Extract base type and flags
  int base_type = type & ~SOCK_NONBLOCK & ~SOCK_CLOEXEC;

  // Validate type
  if (base_type != SOCK_STREAM && base_type != SOCK_DGRAM &&
      base_type != SOCK_RAW && base_type != SOCK_SEQPACKET) {
    return NULL; // EPROTONOSUPPORT
  }

  if (domain == AF_UNIX && protocol != 0) {
    return NULL; // EPROTONOSUPPORT
  }

  // Allocate socket structure
  socket_t *sock = kmalloc(sizeof(socket_t));
  if (!sock) {
    klog_puts("[ERR] socket: failed to allocate socket structure\n");
    return NULL;
  }

  // Initialize socket
  memset(sock, 0, sizeof(socket_t));
  sock->net_ns = net_namespace_get(net_ns);
  sock->domain = domain;
  sock->type = type & ~SOCK_NONBLOCK & ~SOCK_CLOEXEC; // Strip flags
  sock->protocol = protocol;
  sock->state = SS_UNCONNECTED;
  sock->fd = -1;
  sock->flags = type & (SOCK_NONBLOCK | SOCK_CLOEXEC);
  sock->error = 0;
  sock->rcvbuf = SOCKET_DEFAULT_RCVBUF;
  sock->sndbuf = SOCKET_DEFAULT_SNDBUF;
  sock->reuseaddr = 0;
  sock->ops = NULL;
  sock->sk = NULL;
  sock->node = NULL;
  sock->peer = NULL;
  sock->wait_queue = NULL;
  sock->refcount = 1;
  sock->closing = false;
  sock->table_index = -1; // memset left 0, which is a valid table slot
  spinlock_init(&sock->lock);

  // Allocate socket table slot
  int idx = socket_table_alloc();
  if (idx < 0) {
    net_namespace_put(sock->net_ns);
    kfree(sock);
    klog_puts("[ERR] socket: socket table full\n");
    return NULL;
  }

  socket_table[idx] = sock;
  sock->table_index = idx;

  // Initialize wait queue
  socket_wait_queue_init(sock);

  // Create family-specific socket
  net_family_t *family = sock_lookup_family(domain);
  if (sock->net_ns && (domain == AF_INET || domain == AF_INET6)) {
    /* No physical devices or routes exist in this namespace. Transport
     * endpoints must never be inserted into the initial namespace stack. */
    sock->ops = NULL;
  } else if (family && family->create) {
    int ret = family->create(sock, protocol);
    if (ret < 0) {
      socket_table_free(idx);
      sock->wait_queue = NULL; // embedded storage, nothing to free
      net_namespace_put(sock->net_ns);
      kfree(sock);
      return NULL;
    }
  }

  // Socket created successfully
  return sock;
}

void socket_destroy(socket_t *sock) {
  if (!sock)
    return;

  // Call family-specific destroy first without holding sock->lock.
  // Family destroy (e.g. unix_destroy) may need to notify/lock the peer socket.
  // Not holding sock->lock prevents AB-BA deadlocks when peers are destroyed concurrently.
  if (sock->ops && sock->ops->destroy) {
    sock->ops->destroy(sock);
  }

  spinlock_acquire(&sock->lock);

  // Release wait queue (embedded in the socket: wake and detach, no free)
  if (sock->wait_queue) {
    /* Waking is attempted by every family's destroy handler, but do it here as
     * well: this is the last point before the queue goes away, and
     * wait_queue_wake_all() detaches entries so nothing can touch it after. */
    wait_queue_wake_all((wait_queue_t *)sock->wait_queue);
    sock->wait_queue = NULL;
  }

  // NOTE: Family-specific data (sock->sk) is freed by the destroy handler
  // above (e.g., unix_destroy).  Do NOT kfree(sock->sk) here — doing so
  // would be a double-free if the handler already released it.
  sock->sk = NULL;

  // Free VFS node
  if (sock->node) {
    // VFS node cleanup handled elsewhere
    sock->node = NULL;
  }

  sock->peer = NULL;

  spinlock_release(&sock->lock);

  // Free socket table slot. The index is remembered at create time; this
  // used to rescan all SOCKET_MAX_COUNT (4096) slots on every close to find
  // the socket's own slot.
  int idx = sock->table_index;
  if (idx >= 0 && idx < SOCKET_MAX_COUNT && socket_table[idx] == sock) {
    socket_table_free(idx);
  } else {
    for (int i = 0; i < SOCKET_MAX_COUNT; i++) {
      if (socket_table[i] == sock) {
        socket_table_free(i);
        break;
      }
    }
  }

  net_namespace_put(sock->net_ns);
  kfree(sock);
}

void socket_get(socket_t *sock) {
  if (!sock)
    return;
  __atomic_fetch_add(&sock->refcount, 1, __ATOMIC_ACQ_REL);
}

bool socket_try_get(socket_t *sock) {
  if (!sock)
    return false;

  uint64_t refs = __atomic_load_n(&sock->refcount, __ATOMIC_ACQUIRE);
  while (refs != 0) {
    if (__atomic_compare_exchange_n(&sock->refcount, &refs, refs + 1, false,
                                    __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
      return true;
  }

  return false;
}

void socket_put(socket_t *sock) {
  if (!sock)
    return;

  if (__atomic_fetch_sub(&sock->refcount, 1, __ATOMIC_ACQ_REL) == 1) {
    socket_destroy(sock);
  }
}



int socket_bind(socket_t *sock, struct sockaddr *addr, int addrlen) {
  if (!sock || !addr)
    return -22; // EINVAL

  if (sock->net_ns && (sock->domain == AF_INET || sock->domain == AF_INET6)) return -101;

  if (!sock->ops || !sock->ops->bind)
    return -95; // EOPNOTSUPP

  return sock->ops->bind(sock, addr, addrlen);
}

int socket_connect(socket_t *sock, struct sockaddr *addr, int addrlen) {
  if (!sock || !addr)
    return -22; // EINVAL

  if (sock->net_ns && (sock->domain == AF_INET || sock->domain == AF_INET6)) return -101;

  if (!sock->ops || !sock->ops->connect)
    return -95; // EOPNOTSUPP

  return sock->ops->connect(sock, addr, addrlen);
}

int socket_listen(socket_t *sock, int backlog) {
  if (!sock)
    return -22; // EINVAL

  if (sock->net_ns && (sock->domain == AF_INET || sock->domain == AF_INET6)) return -101;

  if (!sock->ops || !sock->ops->listen)
    return -95; // EOPNOTSUPP

  return sock->ops->listen(sock, backlog);
}

int socket_accept(socket_t *sock, socket_t **newsock) {
  if (!sock || !newsock)
    return -22; // EINVAL

  if (sock->net_ns && (sock->domain == AF_INET || sock->domain == AF_INET6)) return -101;

  if (!sock->ops || !sock->ops->accept)
    return -95; // EOPNOTSUPP

  return sock->ops->accept(sock, newsock);
}

ssize_t socket_send(socket_t *sock, const void *buf, size_t len, int flags) {
  if (!sock || !buf)
    return -22; // EINVAL

  if (sock->net_ns && (sock->domain == AF_INET || sock->domain == AF_INET6)) return -101;

  if (!sock->ops || !sock->ops->send)
    return -95; // EOPNOTSUPP

  return sock->ops->send(sock, buf, len, flags);
}

ssize_t socket_recv(socket_t *sock, void *buf, size_t len, int flags) {
  if (len == 0)
    return 0;

  if (!sock || !buf)
    return -22; // EINVAL

  if (sock->net_ns && (sock->domain == AF_INET || sock->domain == AF_INET6)) return -101;

  if (!sock->ops || !sock->ops->recv)
    return -95; // EOPNOTSUPP

  return sock->ops->recv(sock, buf, len, flags);
}

ssize_t socket_sendto(socket_t *sock, const void *buf, size_t len, int flags,
                      struct sockaddr *dest_addr, int addrlen) {
  if (len == 0)
    return 0;

  if (!sock || !buf)
    return -22;

  if (sock->net_ns && (sock->domain == AF_INET || sock->domain == AF_INET6)) return -101;

  if (!sock->ops || !sock->ops->sendto)
    return -95;

  return sock->ops->sendto(sock, buf, len, flags, dest_addr, addrlen);
}

ssize_t socket_recvfrom(socket_t *sock, void *buf, size_t len, int flags,
                        struct sockaddr *src_addr, int *addrlen) {
  if (len == 0)
    return 0;

  if (!sock || !buf)
    return -22;

  if (sock->net_ns && (sock->domain == AF_INET || sock->domain == AF_INET6)) return -101;

  if (!sock->ops || !sock->ops->recvfrom)
    return -95;

  return sock->ops->recvfrom(sock, buf, len, flags, src_addr, addrlen);
}

int socket_getsockname(socket_t *sock, struct sockaddr *addr, int *addrlen) {
  if (!sock) return -9; // EBADF
  if (!sock->ops || !sock->ops->getsockname) return -95; // EOPNOTSUPP
  return sock->ops->getsockname(sock, addr, addrlen);
}

int socket_getpeername(socket_t *sock, struct sockaddr *addr, int *addrlen) {
  if (!sock) return -9; // EBADF
  if (!sock->ops || !sock->ops->getpeername) return -95; // EOPNOTSUPP
  return sock->ops->getpeername(sock, addr, addrlen);
}

// Socketpair Creation

int socket_create_pair(int domain, int type, int protocol, socket_t *sv[2]) {
  // Validate domain
  if (domain != AF_UNIX) {
    return -EAFNOSUPPORT;
  }

  // Extract base type
  int base_type = type & ~SOCK_NONBLOCK & ~SOCK_CLOEXEC;

  // Validate type
  if (base_type != SOCK_STREAM && base_type != SOCK_DGRAM &&
      base_type != SOCK_SEQPACKET) {
    return -EPROTONOSUPPORT;
  }

  // Create two sockets
  socket_t *sock0 = socket_create(domain, type, protocol);
  if (!sock0)
    return -12; // ENOMEM

  socket_t *sock1 = socket_create(domain, type, protocol);
  if (!sock1) {
    socket_put(sock0);
    return -12; // ENOMEM
  }

  // Connect them to each other
  sock0->peer = sock1;
  sock1->peer = sock0;
  sock0->state = SS_CONNECTED;
  sock1->state = SS_CONNECTED;

  // Also connect the AF_UNIX internal peers
  unix_sock_t *usk0 = (unix_sock_t *)sock0->sk;
  unix_sock_t *usk1 = (unix_sock_t *)sock1->sk;
  if (usk0 && usk1) {
    usk0->peer = usk1;
    usk1->peer = usk0;
    usk0->was_connected = true;
    usk1->was_connected = true;
    usk0->peer_cred_pid = usk1->owner_pid;
    usk0->peer_cred_uid = usk1->owner_uid;
    usk0->peer_cred_gid = usk1->owner_gid;
    usk0->peer_cred_valid = true;
    unix_cache_peer_groups(usk0, usk1);
    usk1->peer_cred_pid = usk0->owner_pid;
    usk1->peer_cred_uid = usk0->owner_uid;
    usk1->peer_cred_gid = usk0->owner_gid;
    usk1->peer_cred_valid = true;
    unix_cache_peer_groups(usk1, usk0);
  }

  sv[0] = sock0;
  sv[1] = sock1;

  return 0;
}

// Socket FD Management

int socket_alloc_fd(socket_t *sock) {
  struct thread *t = sched_get_current();
  if (!t)
    return -1;

  int fd = alloc_fd(t);
  if (fd < 0)
    return -24; // EMFILE

  // Create VFS node for socket
  vfs_node_t *node = kmalloc(sizeof(vfs_node_t));
  if (!node) {
    fd_release_reserved(t, fd);
    return -12; // ENOMEM
  }

  vfs_node_init(node);
  node->flags = FS_SOCKET;
  node->inode = (uint32_t)(uint64_t)sock; // Use socket pointer as inode
  node->device = sock;
  node->wait_queue = sock->wait_queue;

  // Set socket VFS operations.
  // NOTE: node->open is intentionally left NULL.  socket_vfs_open calls
  // socket_get, but vfs_close only fires the close handler once (when
  // node->refcount reaches 0).  If node->open were set, fork would call
  // socket_get for every inherited fd, but socket_vfs_close would only run
  // once, leaving sock->refcount permanently elevated after the child exits.
  // Leaving node->open = NULL means sock->refcount stays at 1 (owned by the
  // VFS node) and socket_vfs_close drops it to 0 exactly once — when the
  // last file-table reference (parent or child) is closed.
  node->read = socket_vfs_read;
  node->write = socket_vfs_write;
  node->open = NULL;
  node->close = socket_vfs_close;
  node->poll = socket_vfs_poll;
  node->ioctl = socket_vfs_ioctl;

  sock->fd = fd;
  sock->node = node;
  t->fds[fd] = node;
  t->fd_offsets[fd] = 0;
  t->fd_flags[fd] = O_RDWR | ((sock->flags & SOCK_CLOEXEC) ? FD_FLAGS_CLOEXEC_BIT : 0);

  // node->refcount = 1 (from vfs_node_init) — the fd-table's reference.
  // sock->refcount = 1 (from socket_create) — the VFS node's reference.
  // These are balanced: the last vfs_close fires socket_vfs_close once.

  return fd;
}

/* Return an owned socket reference. Descriptor reservation is not a file,
 * and closing/replacing a descriptor must not free a socket during lookup. */
socket_t *socket_from_fd(int fd) {
  struct thread *t = sched_get_current();
  if (!t || !t->files || fd < 0 || !FD_IN_RANGE(t, (uint32_t)fd))
    return NULL;

  spinlock_acquire(&t->files->lock);
  vfs_node_t *node = t->fds[fd];
  socket_t *sock = NULL;
  if (fd_is_valid_node(node) &&
      (node->flags & FS_TYPE_MASK) == FS_SOCKET) {
    socket_t *candidate = (socket_t *)node->device;
    if (socket_try_get(candidate)) {
      vfs_node_ref(node);
      sock = candidate;
    }
  }
  spinlock_release(&t->files->lock);
  return sock;
}

void socket_fd_put(socket_t *sock) {
  if (!sock)
    return;
  /* The node protects poll/notification hooks until this syscall finishes;
   * the socket reference keeps sock alive while the final node close runs. */
  vfs_close(sock->node);
  socket_put(sock);
}

int socket_close_fd(int fd) {
  struct thread *t = sched_get_current();
  if (!t || !t->files || fd < 0 || !FD_IN_RANGE(t, (uint32_t)fd))
    return -9;
  spinlock_acquire(&t->files->lock);
  vfs_node_t *node = t->fds[fd];
  if (!fd_is_valid_node(node)) {
    spinlock_release(&t->files->lock);
    return -9;
  }
  if ((node->flags & FS_TYPE_MASK) != FS_SOCKET) {
    spinlock_release(&t->files->lock);
    return -22;
  }
  t->fds[fd] = NULL;
  t->fd_offsets[fd] = 0;
  t->fd_flags[fd] = 0;
  fd_path_clear(t, fd);
  if ((uint32_t)fd < t->files->next_fd)
    t->files->next_fd = (uint32_t)fd;
  spinlock_release(&t->files->lock);
  vfs_close(node);
  return 0;
}

// Socket VFS Operations

uint32_t socket_vfs_read(struct vfs_node *node, uint32_t offset, uint32_t size,
                         uint8_t *buffer) {
  (void)offset; // Sockets don't use offset

  if (size == 0)
    return 0;

  if (!node || !buffer)
    return (uint32_t)(int32_t)-14;

  socket_t *sock = (socket_t *)node->device;
  if (!sock)
    return (uint32_t)(int32_t)-9;

  if (!socket_try_get(sock))
    return (uint32_t)(int32_t)-9;
  if (sock->closing) {
    socket_put(sock);
    return 0; // EOF on closed socket
  }

  ssize_t ret = socket_recv(sock, buffer, size, 0);
  socket_put(sock);
  return (uint32_t)ret;
}

uint32_t socket_vfs_write(struct vfs_node *node, uint32_t offset, uint32_t size,
                          uint8_t *buffer) {
  (void)offset; // Sockets don't use offset

  if (size == 0)
    return 0;

  if (!node || !buffer)
    return (uint32_t)(int32_t)-14;

  socket_t *sock = (socket_t *)node->device;
  if (!sock)
    return (uint32_t)(int32_t)-9;

  if (!socket_try_get(sock))
    return (uint32_t)(int32_t)-9;
  if (sock->closing) {
    socket_put(sock);
    return (uint32_t)(int32_t)-32;
  }

  ssize_t ret = socket_send(sock, buffer, size, 0);
  socket_put(sock);
  if (ret == 0 && size > 0)
    return (uint32_t)(int32_t)-11; // EAGAIN
  return (uint32_t)ret;
}

void socket_vfs_open(struct vfs_node *node) {
  if (!node)
    return;
  socket_t *sock = (socket_t *)node->device;
  if (sock)
    socket_get(sock);
}

void socket_vfs_close(struct vfs_node *node) {
  if (!node)
    return;

  socket_t *sock = (socket_t *)node->device;
  if (!sock)
    return;

  socket_t *peer_sock = NULL;

  spinlock_acquire(&sock->lock);
  sock->closing = true;
  if (sock->domain == AF_UNIX && sock->sk) {
    unix_sock_t *usk = (unix_sock_t *)sock->sk;
    if (usk->peer && usk->peer->parent && socket_try_get(usk->peer->parent))
      peer_sock = usk->peer->parent;
  }
  spinlock_release(&sock->lock);

  if (sock->wait_queue)
    wait_queue_wake_all((wait_queue_t *)sock->wait_queue);

  if (sock->domain == AF_UNIX && sock->sk) {
    unix_sock_t *usk = (unix_sock_t *)sock->sk;
    if (usk->wait)
      wait_queue_wake_all(usk->wait);
  }

  if (peer_sock) {
    unix_sock_t *peer = (unix_sock_t *)peer_sock->sk;
    if (peer && peer->wait)
      wait_queue_wake_all(peer->wait);
    socket_put(peer_sock);
  }

  socket_put(sock);
}

int socket_vfs_poll(struct vfs_node *node, int events) {
  if (!node)
    return POLLNVAL;

  socket_t *sock = (socket_t *)node->device;
  if (!sock)
    return POLLNVAL;

  if (!socket_try_get(sock))
    return POLLNVAL;
  if (sock->closing) {
    socket_put(sock);
    return POLLHUP;
  }

  if (sock->ops && sock->ops->poll) {
    int ret = sock->ops->poll(sock, events);
    socket_put(sock);
    return ret;
  }

  // Default: socket is always writable if connected
  int revents = 0;
  if (socket_is_connected(sock)) {
    if (events & POLLOUT)
      revents |= POLLOUT;
  }
  socket_put(sock);
  return revents;
}

int socket_vfs_ioctl(struct vfs_node *node, uint32_t request, uint64_t arg) {
  if (!node)
    return -25; // ENOTTY

  socket_t *sock = (socket_t *)node->device;
  if (!sock)
    return -25; // ENOTTY

  if (!socket_try_get(sock))
    return -9;
  if (sock->closing) {
    socket_put(sock);
    return -9;
  }

  if (sock->net_ns && (request >= 0x8910 && request <= 0x89ff)) {
    int ret = -19;
    if (request == 0x8912) {
      struct avory_ifconf64 conf;
      if (copy_from_user(&conf, (void *)arg, sizeof(conf))) ret = -14;
      else {
        if (conf.length < 0) ret = -22;
        else {
          bool configured = sock->net_ns->loopback_address != 0;
          struct avory_ifreq64 loop = {0}; memcpy(loop.name, "lo", 3);
          loop.family = 2; loop.address_data[2] = 127; loop.address_data[5] = 1;
          if (!conf.buffer) conf.length = configured ? sizeof(loop) : 0;
          else if (configured && conf.length >= (int)sizeof(loop)) {
            ret = copy_to_user((void *)conf.buffer, &loop, sizeof(loop)) ? -14 : 0;
            conf.length = ret ? 0 : sizeof(loop);
          } else conf.length = 0;
          if (ret != -14) ret = copy_to_user((void *)arg, &conf, sizeof(conf)) ? -14 : 0;
        }
      }
    } else {
      struct avory_ifreq64 req;
      if (copy_from_user(&req, (void *)arg, sizeof(req))) ret = -14;
      else if ((request == 0x8910 && req.family == 1 && req.address_data[0] == 0 && req.address_data[1] == 0) ||
               socket_ifreq_name_is(req.name, "lo")) {
        ret = 0;
        if (request == 0x8910) { memset(req.name, 0, 16); memcpy(req.name, "lo", 3); }
        else if (request == 0x8933) { uint32_t index = 1; memcpy(&req.family, &index, 4); }
        else if (request == 0x8913) req.family = sock->net_ns->loopback_flags;
        else if (request == 0x8914) {
          if (!capability_has(sched_get_current(), CAP_NET_ADMIN)) ret = -1;
          else __atomic_store_n(&sock->net_ns->loopback_flags, (uint32_t)(req.family & 1) | 8, __ATOMIC_RELEASE);
        } else if (request == 0x8915 && sock->net_ns->loopback_address) {
          memset(&req.family, 0, sizeof(req) - 16); req.family = 2;
          req.address_data[2] = 127; req.address_data[5] = 1;
        } else ret = request == 0x8915 ? -99 : -25;
        if (!ret && copy_to_user((void *)arg, &req, sizeof(req))) ret = -14;
      }
    }
    socket_put(sock); return ret;
  }

  /* SIOCGIFCONF is a socket ioctl used by libc and clients to enumerate
   * configured IPv4 interfaces. The network stack has one IPv4 config tied
   * to its default device, which is the complete set currently available. */
  if (request == 0x8912) {
    int ret = socket_ioctl_ifconf(arg);
    socket_put(sock);
    return ret;
  }
  if (request == 0x8910 || request == 0x8915 || request == 0x8933) {
    int ret = socket_ioctl_ifreq(request, arg);
    socket_put(sock);
    return ret;
  }

  /* FIONBIO is a generic socket ioctl. Rust std/rustix uses it to put TCP
   * sockets into nonblocking mode before connect(2), so handling it only in
   * AF_UNIX makes AF_INET and AF_INET6 fail with ENOTTY. */
  if (request == 0x5421) { /* FIONBIO */
    if (!arg || !vmm_is_user_addr_range_valid(arg, sizeof(int))) {
      socket_put(sock);
      return -14; /* EFAULT */
    }

    int nonblocking;
    memcpy(&nonblocking, (const void *)arg, sizeof(nonblocking));
    spinlock_acquire(&sock->lock);
    if (nonblocking)
      sock->flags |= SOCK_NONBLOCK;
    else
      sock->flags &= ~SOCK_NONBLOCK;
    spinlock_release(&sock->lock);

    socket_put(sock);
    return 0;
  }

  if (sock->ops && sock->ops->ioctl) {
    int ret = sock->ops->ioctl(sock, request, arg);
    if (ret != -25 || request != 0x541B) {
      socket_put(sock);
      return ret;
    }
  }

  if (request == 0x541B) { /* FIONREAD / TIOCINQ fallback */
    if (!arg || !vmm_is_user_addr_range_valid(arg, sizeof(int))) {
      socket_put(sock);
      return -14;
    }
    int unread = 0;
    if (copy_to_user((void *)arg, &unread, sizeof(int)) != 0) {
      socket_put(sock);
      return -14;
    }
    socket_put(sock);
    return 0;
  }

  socket_put(sock);
  return -25; // ENOTTY
}

// Family Registration

void sock_register_family(net_family_t *family) {
  if (!family)
    return;

  family->next = family_registry;
  family_registry = family;
}

net_family_t *sock_lookup_family(int family) {
  net_family_t *f = family_registry;
  while (f) {
    if (f->family == family)
      return f;
    f = f->next;
  }
  return NULL;
}

// Socket Subsystem Initialization

void socket_init(void) {
  // Initialize socket table
  memset(socket_table, 0, sizeof(socket_table));
  socket_count = 0;
  spinlock_init(&socket_table_lock);

  // Register socket families
  af_unix_init();
  af_netlink_init();
  af_inet_init();
  af_inet6_init();
  af_qrtr_init();

  klog_puts("[OK] Socket subsystem initialized (max sockets: ");
  klog_uint64(SOCKET_MAX_COUNT);
  klog_puts(")\n");
}
