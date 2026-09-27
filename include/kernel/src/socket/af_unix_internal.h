#ifndef AF_UNIX_INTERNAL_H
#define AF_UNIX_INTERNAL_H

// Internal header shared between af_unix_*.c sub-modules.
// Do NOT include from outside the socket/ directory.

#include "af_unix.h"
#include "../console/klog.h"
#include "../cpu/ktrack.h"
#include "../fs/vfs.h"
#include "../lib/list.h"
#include "../lib/string.h"
#include "../mm/heap.h"
#include "../mm/vmm.h"
#include "../sched/sched.h"
#include "epoll.h"
#include "socket.h"
#include "socket_internal.h"
#include "../include/arch/uaccess.h"
#include <stddef.h>
#include <stdint.h>

// ── Stream ring buffer helpers ───────────────────────────────────────────────
// The stream receive ring is a plain circular buffer holding at most size-1
// bytes, so any contiguous run copied in or out wraps at most once.  These two
// helpers replace the per-byte `% size` loops that previously dominated
// AF_UNIX throughput (a 64-bit hardware divide for every single byte).  X11
// carries all window pixel traffic over AF_UNIX when MIT-SHM is unavailable,
// so this is the hottest path in the entire desktop session.
//
// Callers must hold the owning socket's recv_lock and must guarantee that
// `len` fits: len <= free space when appending, len <= available when
// consuming.  That bound is what makes one conditional subtract enough to
// wrap, instead of a second divide.

// Append `len` bytes of the linear buffer `src` into `buf` at `tail`.
static inline size_t unix_ring_append(uint8_t *buf, size_t size, size_t tail,
                                      const uint8_t *src, size_t len) {
  if (!buf || size == 0 || len == 0)
    return tail;
  size_t first = size - tail;
  if (first > len)
    first = len;
  memcpy(buf + tail, src, first);
  if (len > first)
    memcpy(buf, src + first, len - first);
  tail += len;
  return tail >= size ? tail - size : tail;
}

// Copy `len` bytes out of `buf` at `head` into the linear buffer `dst`.
static inline size_t unix_ring_consume(const uint8_t *buf, size_t size, size_t head,
                                       uint8_t *dst, size_t len) {
  if (!buf || size == 0 || len == 0)
    return head;
  size_t first = size - head;
  if (first > len)
    first = len;
  memcpy(dst, buf + head, first);
  if (len > first)
    memcpy(dst + first, buf, len - first);
  head += len;
  return head >= size ? head - size : head;
}

// ── Bound-socket registry (owned by af_unix_addr.c) ──────────────────────────
extern struct list_head unix_bound_list;
extern spinlock_t       unix_bound_lock;

// ── Internal function declarations ───────────────────────────────────────────

// af_unix_addr.c
unix_sock_t *unix_find_socket_by_addr(struct sockaddr_un *addr, int addrlen);
unix_sock_t *unix_find_socket_by_addr_ref(struct sockaddr_un *addr, int addrlen);
bool af_unix_sock_live(unix_sock_t *usk, socket_t **sock_out);

// af_unix_bind.c  (unix_bind is the public ops entry — kept static there)

// af_unix_connect.c
// unix_connect / unix_accept / unix_listen are static, registered via unix_ops

// af_unix_io.c
// all send/recv variants are static, registered via unix_ops

// af_unix_sockopt.c
// getsockopt / setsockopt are static, registered via unix_ops

#endif // AF_UNIX_INTERNAL_H
