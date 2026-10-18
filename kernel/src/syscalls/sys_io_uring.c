// sys_io_uring.c — Linux-compatible io_uring syscalls (425..427).
//
// Minimal but functional implementation: a per-open ring context with
// PMM-backed SQ/CQ rings and SQE array shared with userspace through the
// node's mmap handler, synchronous in-kernel execution on io_uring_enter,
// and registered buffers/files/eventfd support.
//
// Execution is synchronous (no SQPOLL kernel thread, no IOPOLL): each
// io_uring_enter consumes newly submitted SQEs in submission order and
// posts one CQE per SQE. Unsupported opcodes complete with -EINVAL in
// their CQE (Linux-compatible per-op failure, not a syscall failure), so
// callers using feature probes (e.g. Chromium/Electron io_uring detection
// via IORING_REGISTER_PROBE) see an honest capability set.
//
// Locking: claim/execute/ordered-complete. The consume phase copies SQEs
// under the context lock, execution runs without it (VFS ops may block),
// and the complete phase writes CQEs in claim order via a ticket so
// concurrent enters cannot reorder completions.
#include "sys_io_shared.h"
#include "../console/klog.h"
#include "../fs/ramfs.h"
#include "../lib/string.h"
#include "../mm/heap.h"
#include "../mm/pmm.h"
#include "../mm/vmm.h"
#include "../sched/sched.h"
#include "../sched/wait.h"
#include "syscall.h"
#include <stdint.h>

extern void epoll_notify_event(struct vfs_node *node, uint32_t events);

// ---------------------------------------------------------------------------
// UAPI (matches Linux include/uapi/linux/io_uring.h layout)
// ---------------------------------------------------------------------------

#define IORING_OFF_SQ_RING 0ULL
#define IORING_OFF_CQ_RING 0x8000000ULL
#define IORING_OFF_SQES 0x10000000ULL

#define IORING_SETUP_IOPOLL 1U
#define IORING_SETUP_SQPOLL 2U
#define IORING_SETUP_SQ_AFF 4U
#define IORING_SETUP_CQSIZE 8U
#define IORING_SETUP_CLAMP 16U
#define IORING_SETUP_ATTACH_WQ 32U
#define IORING_SETUP_R_DISABLED 64U
#define IORING_SETUP_SQE128 1024U
#define IORING_SETUP_CQE32 2048U
#define IORING_SETUP_SINGLE_ISSUER 4096U
#define IORING_SETUP_DEFER_TASKRUN 8192U

#define IORING_FEAT_SINGLE_MMAP 0U // deliberately not advertised (3 mappings)

#define IORING_ENTER_GETEVENTS 1U
#define IORING_ENTER_SQ_WAKEUP 2U
#define IORING_ENTER_SQ_WAIT 4U
#define IORING_ENTER_EXT_ARG 8U

#define IOSQE_FIXED_FILE (1U << 0)
#define IOSQE_ASYNC (1U << 4)

struct io_sqring_offsets {
  uint32_t head, tail, ring_mask, ring_entries, flags, dropped, array, resv1;
  uint64_t user_addr;
};

struct io_cqring_offsets {
  uint32_t head, tail, ring_mask, ring_entries, overflow, cqes, flags, resv1;
  uint64_t user_addr;
};

struct io_uring_params {
  uint32_t sq_entries, cq_entries, flags, sq_thread_cpu, sq_thread_idle,
      features, wq_fd, resv[3];
  struct io_sqring_offsets sq_off;
  struct io_cqring_offsets cq_off;
};

struct io_uring_sqe {
  uint8_t opcode, flags;
  uint16_t ioprio;
  int32_t fd;
  uint64_t off;
  uint64_t addr;
  uint32_t len;
  uint32_t op_flags;
  uint64_t user_data;
  uint16_t buf_index, personality;
  int32_t splice_fd_in;
  union {
    struct {
      uint16_t poll_cond, poll_retries;
    };
    uint64_t pad2;
  };
  uint64_t addr3;
};
_Static_assert(sizeof(struct io_uring_sqe) == 64, "sqe layout");

struct io_uring_cqe {
  uint64_t user_data;
  int32_t res;
  uint32_t flags;
};
_Static_assert(sizeof(struct io_uring_cqe) == 16, "cqe layout");

struct io_uring_probe_op {
  uint8_t op, resv;
  uint16_t flags;
  uint32_t resv2;
};

struct io_uring_probe {
  uint8_t last_op, ops_len;
  uint16_t resv;
  uint32_t resv2[3];
  struct io_uring_probe_op ops[];
};

struct io_uring_files_update {
  uint32_t offset, resv;
  uint64_t fds;
};

enum {
  IORING_OP_NOP = 0,
  IORING_OP_READV = 1,
  IORING_OP_WRITEV = 2,
  IORING_OP_FSYNC = 3,
  IORING_OP_READ_FIXED = 4,
  IORING_OP_WRITE_FIXED = 5,
  IORING_OP_POLL_ADD = 6,
  IORING_OP_POLL_REMOVE = 7,
  IORING_OP_SYNC_FILE_RANGE = 8,
  IORING_OP_SENDMSG = 9,
  IORING_OP_RECVMSG = 10,
  IORING_OP_OPENAT = 18,
  IORING_OP_CLOSE = 19,
  IORING_OP_STATX = 21,
  IORING_OP_READ = 22,
  IORING_OP_WRITE = 23,
  // One past the highest opcode known here; resync with Linux
  // include/uapi/linux/io_uring.h (enum io_uring_op) when adding ops.
  IORING_OP_LAST = 65,
};

#define IO_URING_OP_SUPPORTED (1U << 0)

enum {
  IORING_REGISTER_BUFFERS = 0,
  IORING_UNREGISTER_BUFFERS = 1,
  IORING_REGISTER_FILES = 2,
  IORING_UNREGISTER_FILES = 3,
  IORING_REGISTER_EVENTFD = 4,
  IORING_UNREGISTER_EVENTFD = 5,
  IORING_REGISTER_FILES_UPDATE = 6,
  IORING_REGISTER_EVENTFD_ASYNC = 7,
  IORING_REGISTER_PROBE = 8,
  IORING_REGISTER_ENABLE_RINGS = 12,
  IORING_REGISTER_RESIZE_RINGS = 20,
};

#define IORING_MAX_ENTRIES 4096
#define IORING_MAX_IOV 1024

// ---------------------------------------------------------------------------
// Context
// ---------------------------------------------------------------------------

#define IORING_MAGIC 0x7572696fU // "urio"

typedef struct {
  uint32_t magic;
  spinlock_t lock;
  wait_queue_t wq;
  vfs_node_t *node;
  uint64_t seq_claim, seq_complete; // CQE ordering tickets
  uint32_t sq_entries, cq_entries, sq_mask, cq_mask;
  uint32_t khead; // kernel-consumed SQ index
  // Shared rings (PMM pages, kernel VA via HHDM).
  uint64_t sq_ring_phys, cq_ring_phys, sqe_phys;
  uint32_t sq_ring_pages, cq_ring_pages, sqe_pages;
  volatile uint32_t *sq_head, *sq_tail, *sq_mask_p, *sq_flags, *sq_dropped;
  volatile uint32_t *sq_array;
  volatile uint32_t *cq_head, *cq_tail, *cq_overflow;
  volatile struct io_uring_cqe *cqes;
  struct io_uring_sqe *sqes;
  // Registrations.
  struct user_iovec *reg_bufs;
  uint32_t reg_buf_count;
  vfs_node_t **reg_files;
  uint32_t reg_file_count;
  vfs_node_t *eventfd_node;
  bool rings_disabled;
} ioring_ctx_t;

static uint32_t ioring_pow2(uint32_t v) {
  if (v < 8)
    return 8;
  v--;
  v |= v >> 1;
  v |= v >> 2;
  v |= v >> 4;
  v |= v >> 8;
  v |= v >> 16;
  return v + 1;
}

static uint64_t ioring_alloc_region(uint32_t pages, uint64_t *phys_out) {
  void *phys = pmm_alloc_pages(pages);
  if (!phys)
    return 0;
  uint64_t hhdm = pmm_get_hhdm_offset();
  memset((void *)((uint64_t)phys + hhdm), 0, (size_t)pages * PAGE_SIZE);
  *phys_out = (uint64_t)phys;
  return (uint64_t)phys + hhdm;
}

static void ioring_free_ctx_rings(ioring_ctx_t *ctx) {
  if (ctx->sq_ring_phys)
    pmm_free_pages((void *)ctx->sq_ring_phys, ctx->sq_ring_pages);
  if (ctx->cq_ring_phys)
    pmm_free_pages((void *)ctx->cq_ring_phys, ctx->cq_ring_pages);
  if (ctx->sqe_phys)
    pmm_free_pages((void *)ctx->sqe_phys, ctx->sqe_pages);
  ctx->sq_ring_phys = ctx->cq_ring_phys = ctx->sqe_phys = 0;
}

// Offsets reported to userspace (byte offsets inside each ring mapping).
#define IORING_SQ_HEAD_OFF 0U
#define IORING_SQ_TAIL_OFF 64U
#define IORING_SQ_MASK_OFF 128U
#define IORING_SQ_ENTRIES_OFF 192U
#define IORING_SQ_FLAGS_OFF 256U
#define IORING_SQ_DROPPED_OFF 320U
#define IORING_SQ_ARRAY_OFF 384U
#define IORING_CQ_HEAD_OFF 0U
#define IORING_CQ_TAIL_OFF 64U
#define IORING_CQ_MASK_OFF 128U
#define IORING_CQ_ENTRIES_OFF 192U
#define IORING_CQ_OVERFLOW_OFF 256U
#define IORING_CQ_CQES_OFF 320U
#define IORING_CQ_FLAGS_OFF 384U

static volatile uint32_t *ioring_at(uint64_t base, uint32_t off) {
  return (volatile uint32_t *)(base + off);
}

// ---------------------------------------------------------------------------
// Node ops: poll / mmap / close
// ---------------------------------------------------------------------------

static int ioring_poll(vfs_node_t *node, int events) {
  ioring_ctx_t *ctx = node ? (ioring_ctx_t *)node->device : NULL;
  if (!ctx || ctx->magic != IORING_MAGIC)
    return 0;
  spinlock_acquire(&ctx->lock);
  uint32_t head = __atomic_load_n((uint32_t *)ctx->cq_head, __ATOMIC_ACQUIRE);
  uint32_t tail = __atomic_load_n((uint32_t *)ctx->cq_tail, __ATOMIC_ACQUIRE);
  uint32_t sq_head = ctx->khead;
  uint32_t sq_tail = __atomic_load_n((uint32_t *)ctx->sq_tail, __ATOMIC_ACQUIRE);
  spinlock_release(&ctx->lock);
  int revents = 0;
  if (head != tail)
    revents |= (POLLIN | POLLRDNORM);
  if (sq_tail - sq_head < ctx->sq_entries)
    revents |= (POLLOUT | POLLWRNORM);
  return revents & events;
}

static uint64_t ioring_mmap(vfs_node_t *node, uint64_t addr, uint64_t length,
                            uint64_t prot, uint64_t flags, uint64_t offset) {
  (void)flags;
  ioring_ctx_t *ctx = node ? (ioring_ctx_t *)node->device : NULL;
  if (!ctx || ctx->magic != IORING_MAGIC || !length)
    return (uint64_t)-22;
  // Rings are shared state: a private mapping would silently stop
  // propagating head/tail updates. Linux requires MAP_SHARED here too.
  if ((flags & 0x3) == 0x02 /* MAP_PRIVATE */)
    return (uint64_t)-22;
  uint64_t phys = 0;
  uint64_t region_size = 0;
  if (offset == IORING_OFF_SQ_RING) {
    phys = ctx->sq_ring_phys;
    region_size = (uint64_t)ctx->sq_ring_pages * PAGE_SIZE;
  } else if (offset == IORING_OFF_CQ_RING) {
    phys = ctx->cq_ring_phys;
    region_size = (uint64_t)ctx->cq_ring_pages * PAGE_SIZE;
  } else if (offset == IORING_OFF_SQES) {
    phys = ctx->sqe_phys;
    region_size = (uint64_t)ctx->sqe_pages * PAGE_SIZE;
  } else {
    return (uint64_t)-22;
  }
  if (!phys || length > region_size)
    return (uint64_t)-22;
  uint64_t vaddr = addr ? addr : mm_alloc_mmap_region(length);
  if (!vaddr)
    return (uint64_t)-12;
  uint64_t page_flags = PAGE_FLAG_PRESENT | PAGE_FLAG_USER;
  if (prot & 0x2)
    page_flags |= PAGE_FLAG_RW;
  if (!(prot & 0x4))
    page_flags |= PAGE_FLAG_NX;
  uint64_t *pml4 = vmm_get_active_pml4();
  for (uint64_t off = 0; off < length; off += PAGE_SIZE)
    vmm_map_page(pml4, vaddr + off, phys + off, page_flags);
  return vaddr;
}

static void ioring_close(vfs_node_t *node) {
  if (!node || !node->device)
    return;
  ioring_ctx_t *ctx = (ioring_ctx_t *)node->device;
  if (ctx->magic != IORING_MAGIC)
    return;
  wait_queue_wake_all(&ctx->wq);
  for (uint32_t i = 0; i < ctx->reg_file_count; i++)
    if (ctx->reg_files[i])
      vfs_close(ctx->reg_files[i]);
  if (ctx->reg_files)
    kfree(ctx->reg_files);
  if (ctx->reg_bufs)
    kfree(ctx->reg_bufs);
  if (ctx->eventfd_node)
    vfs_close(ctx->eventfd_node);
  ioring_free_ctx_rings(ctx);
  node->device = NULL;
  kfree(ctx);
}

// ---------------------------------------------------------------------------
// Op execution (no context lock held; SQE already copied out)
// ---------------------------------------------------------------------------

static vfs_node_t *ioring_pin_op_fd(struct thread *t, ioring_ctx_t *ctx,
                                    const struct io_uring_sqe *sqe) {
  if (sqe->flags & IOSQE_FIXED_FILE) {
    uint32_t idx = (uint32_t)sqe->fd;
    spinlock_acquire(&ctx->lock);
    vfs_node_t *node =
        (idx < ctx->reg_file_count) ? ctx->reg_files[idx] : NULL;
    if (node)
      vfs_node_ref(node);
    spinlock_release(&ctx->lock);
    return node;
  }
  if (sqe->fd < 0)
    return NULL;
  return fd_node_get(t, (uint64_t)(uint32_t)sqe->fd);
}

static int32_t ioring_do_readv(struct thread *t, ioring_ctx_t *ctx,
                               const struct io_uring_sqe *sqe, bool fixed) {
  (void)ctx;
  if (sqe->len > IORING_MAX_IOV)
    return -22;
  vfs_node_t *node __attribute__((cleanup(fd_node_put))) =
      ioring_pin_op_fd(t, ctx, sqe);
  if (!node)
    return -9;
  uint64_t off = sqe->off;
  int64_t total = 0;
  if (fixed) {
    // addr/len must sit inside the selected registered buffer.
    spinlock_acquire(&ctx->lock);
    bool ok = sqe->buf_index < ctx->reg_buf_count;
    uint64_t bbase = 0, blen = 0;
    if (ok) {
      bbase = ctx->reg_bufs[sqe->buf_index].iov_base;
      blen = ctx->reg_bufs[sqe->buf_index].iov_len;
    }
    spinlock_release(&ctx->lock);
    if (!ok)
      return -9;
    if (sqe->addr < bbase || sqe->len > blen || sqe->addr + sqe->len > bbase + blen)
      return -14;
    if (!vmm_is_user_addr_range_valid(sqe->addr, sqe->len))
      return -14;
    if (off > UINT32_MAX || sqe->len > UINT32_MAX)
      return -22;
    int32_t r = (int32_t)vfs_read(node, (uint32_t)off, sqe->len,
                                  (uint8_t *)sqe->addr);
    return r < 0 ? r : r;
  }
  if (sqe->len == 0)
    return 0;
  if (!vmm_is_user_addr_range_valid(sqe->addr,
                                   sqe->len * sizeof(struct user_iovec)))
    return -14;
  uint32_t count = sqe->len;
  struct user_iovec stack[16];
  struct user_iovec *iov = stack;
  bool heap = false;
  if (count > 16) {
    iov = kmalloc(count * sizeof(struct user_iovec));
    if (!iov)
      return -12;
    heap = true;
  }
  if (copy_from_user(iov, (const void *)sqe->addr,
                     count * sizeof(struct user_iovec)) != 0) {
    if (heap)
      kfree(iov);
    return -14;
  }
  for (uint32_t i = 0; i < count; i++) {
    if (iov[i].iov_len == 0)
      continue;
    if (!vmm_is_user_addr_range_valid(iov[i].iov_base, iov[i].iov_len) ||
        off > UINT32_MAX || iov[i].iov_len > UINT32_MAX) {
      if (!total)
        total = -14;
      break;
    }
    int32_t r = (int32_t)vfs_read(node, (uint32_t)off, (uint32_t)iov[i].iov_len,
                                  (uint8_t *)iov[i].iov_base);
    if (r < 0) {
      if (!total)
        total = r;
      break;
    }
    total += r;
    off += (uint64_t)r;
    if ((uint32_t)r < iov[i].iov_len)
      break;
  }
  if (heap)
    kfree(iov);
  return (int32_t)total;
}

static int32_t ioring_do_writev(struct thread *t, ioring_ctx_t *ctx,
                                const struct io_uring_sqe *sqe, bool fixed) {
  (void)ctx;
  if (!fixed && sqe->len > IORING_MAX_IOV)
    return -22;
  vfs_node_t *node __attribute__((cleanup(fd_node_put))) =
      ioring_pin_op_fd(t, ctx, sqe);
  if (!node)
    return -9;
  uint64_t off = sqe->off;
  if (fixed) {
    spinlock_acquire(&ctx->lock);
    bool ok = sqe->buf_index < ctx->reg_buf_count;
    uint64_t bbase = 0, blen = 0;
    if (ok) {
      bbase = ctx->reg_bufs[sqe->buf_index].iov_base;
      blen = ctx->reg_bufs[sqe->buf_index].iov_len;
    }
    spinlock_release(&ctx->lock);
    if (!ok)
      return -9;
    if (sqe->addr < bbase || sqe->len > blen || sqe->addr + sqe->len > bbase + blen)
      return -14;
    if (!vmm_is_user_addr_range_valid(sqe->addr, sqe->len))
      return -14;
    if (off > UINT32_MAX || sqe->len > UINT32_MAX)
      return -22;
    return (int32_t)vfs_write(node, (uint32_t)off, sqe->len,
                              (uint8_t *)sqe->addr);
  }
  if (sqe->len == 0)
    return 0;
  if (!vmm_is_user_addr_range_valid(sqe->addr,
                                   sqe->len * sizeof(struct user_iovec)))
    return -14;
  uint32_t count = sqe->len;
  struct user_iovec stack[16];
  struct user_iovec *iov = stack;
  bool heap = false;
  if (count > 16) {
    iov = kmalloc(count * sizeof(struct user_iovec));
    if (!iov)
      return -12;
    heap = true;
  }
  if (copy_from_user(iov, (const void *)sqe->addr,
                     count * sizeof(struct user_iovec)) != 0) {
    if (heap)
      kfree(iov);
    return -14;
  }
  int64_t total = 0;
  for (uint32_t i = 0; i < count; i++) {
    if (iov[i].iov_len == 0)
      continue;
    if (!vmm_is_user_addr_range_valid(iov[i].iov_base, iov[i].iov_len) ||
        off > UINT32_MAX || iov[i].iov_len > UINT32_MAX) {
      if (!total)
        total = -14;
      break;
    }
    int32_t r = (int32_t)vfs_write(node, (uint32_t)off,
                                   (uint32_t)iov[i].iov_len,
                                   (uint8_t *)iov[i].iov_base);
    if (r < 0) {
      if (!total)
        total = r;
      break;
    }
    total += r;
    off += (uint64_t)r;
    if ((uint32_t)r < iov[i].iov_len)
      break;
  }
  if (heap)
    kfree(iov);
  return (int32_t)total;
}

static int32_t ioring_do_read_write(struct thread *t, ioring_ctx_t *ctx,
                                    const struct io_uring_sqe *sqe, bool write) {
  vfs_node_t *node __attribute__((cleanup(fd_node_put))) =
      ioring_pin_op_fd(t, ctx, sqe);
  if (!node)
    return -9;
  if (sqe->len == 0)
    return 0;
  if (!vmm_is_user_addr_range_valid(sqe->addr, sqe->len))
    return -14;
  if (sqe->off > UINT32_MAX || sqe->len > UINT32_MAX)
    return -22;
  if (write)
    return (int32_t)vfs_write(node, (uint32_t)sqe->off, sqe->len,
                              (uint8_t *)sqe->addr);
  return (int32_t)vfs_read(node, (uint32_t)sqe->off, sqe->len,
                           (uint8_t *)sqe->addr);
}

static int32_t ioring_do_poll(struct thread *t, ioring_ctx_t *ctx,
                              const struct io_uring_sqe *sqe) {
  vfs_node_t *node __attribute__((cleanup(fd_node_put))) =
      ioring_pin_op_fd(t, ctx, sqe);
  if (!node)
    return -9;
  if (!node->poll)
    return 0;
  int events = sqe->op_flags & 0xffff;
  if (!events)
    events = POLLIN | POLLERR;
  return (int32_t)node->poll(node, events);
}

static int32_t ioring_do_openat(struct thread *t,
                                const struct io_uring_sqe *sqe) {
  (void)t;
  if (!sqe->addr)
    return -14;
  char *path = kmalloc(4096);
  if (!path)
    return -12;
  long n = strncpy_from_user(path, (const char *)sqe->addr, 4096);
  if (n < 0) {
    kfree(path);
    return -14;
  }
  if ((size_t)n >= 4096) {
    kfree(path);
    return -36;
  }
  int dirfd = sqe->fd;
  uint64_t flags = sqe->op_flags;
  uint64_t mode = sqe->len;
  // sys_open_path resolves relative to dirfd (or CWD for AT_FDCWD).
  uint64_t r = sys_open_path(dirfd, path, flags, mode);
  kfree(path);
  if (r >= (uint64_t)-4095)
    return (int32_t)(int64_t)r;
  return (int32_t)r;
}

static int32_t ioring_do_close(struct thread *t, ioring_ctx_t *ctx,
                               const struct io_uring_sqe *sqe) {
  (void)ctx;
  int fd = sqe->fd;
  if (fd < 0 || fd >= MAX_FDS || !t || !t->files)
    return -9;
  spinlock_acquire(&t->files->lock);
  if ((uint32_t)fd >= t->files->capacity) {
    spinlock_release(&t->files->lock);
    return -9;
  }
  vfs_node_t *node = t->fds[fd];
  if (!fd_is_valid_node(node)) {
    spinlock_release(&t->files->lock);
    return -9;
  }
  t->fds[fd] = NULL;
  t->fd_offsets[fd] = 0;
  t->fd_flags[fd] = 0;
  fd_path_clear(t, fd);
  if (fd < (int)t->files->next_fd)
    t->files->next_fd = (uint32_t)fd;
  spinlock_release(&t->files->lock);
  vfs_close(node);
  return 0;
}

static int32_t ioring_execute(struct thread *t, ioring_ctx_t *ctx,
                              const struct io_uring_sqe *sqe) {
  switch (sqe->opcode) {
  case IORING_OP_NOP:
    return 0;
  case IORING_OP_READV:
    return ioring_do_readv(t, ctx, sqe, false);
  case IORING_OP_WRITEV:
    return ioring_do_writev(t, ctx, sqe, false);
  case IORING_OP_READ_FIXED:
    return ioring_do_readv(t, ctx, sqe, true);
  case IORING_OP_WRITE_FIXED:
    return ioring_do_writev(t, ctx, sqe, true);
  case IORING_OP_READ:
    return ioring_do_read_write(t, ctx, sqe, false);
  case IORING_OP_WRITE:
    return ioring_do_read_write(t, ctx, sqe, true);
  case IORING_OP_FSYNC:
  case IORING_OP_SYNC_FILE_RANGE:
    return 0;
  case IORING_OP_POLL_ADD:
    return ioring_do_poll(t, ctx, sqe);
  case IORING_OP_POLL_REMOVE:
    return -2; // ENOENT: nothing stays pending in sync execution
  case IORING_OP_OPENAT:
    return ioring_do_openat(t, sqe);
  case IORING_OP_CLOSE:
    return ioring_do_close(t, ctx, sqe);
  default:
    return -22; // EINVAL: op not implemented yet
  }
}

// ---------------------------------------------------------------------------
// Syscalls
// ---------------------------------------------------------------------------

static uint64_t sys_io_uring_setup(uint64_t entries, uint64_t params_ptr,
                                   uint64_t a2, uint64_t a3, uint64_t a4,
                                   uint64_t a5) {
  (void)a2;
  (void)a3;
  (void)a4;
  (void)a5;
  struct thread *t = sched_get_current();
  if (!t)
    return (uint64_t)-1;
  if (!entries || entries > IORING_MAX_ENTRIES)
    return (uint64_t)-22;
  if (!params_ptr ||
      !vmm_is_user_addr_range_valid(params_ptr, sizeof(struct io_uring_params)))
    return (uint64_t)-14;

  struct io_uring_params p;
  if (copy_from_user(&p, (const void *)params_ptr, sizeof(p)) != 0)
    return (uint64_t)-14;

  const uint32_t supported_flags = IORING_SETUP_CQSIZE | IORING_SETUP_CLAMP;
  if ((p.flags & ~supported_flags) || p.sq_thread_cpu || p.sq_thread_idle ||
      p.wq_fd || p.resv[0] || p.resv[1] || p.resv[2])
    return (uint64_t)-22;

  uint32_t sq_entries = p.sq_entries ? p.sq_entries : (uint32_t)entries;
  if (!sq_entries || sq_entries > IORING_MAX_ENTRIES)
    return (uint64_t)-22;
  sq_entries = ioring_pow2(sq_entries);
  if (sq_entries > IORING_MAX_ENTRIES)
    return (uint64_t)-22;
  uint32_t cq_entries = (p.flags & IORING_SETUP_CQSIZE) && p.cq_entries
                            ? p.cq_entries
                            : sq_entries * 2;
  if (!cq_entries || cq_entries < sq_entries)
    return (uint64_t)-22;
  cq_entries = ioring_pow2(cq_entries);
  if (cq_entries > IORING_MAX_ENTRIES * 2)
    return (uint64_t)-22;

  int fd = alloc_fd(t);
  if (fd < 0)
    return (uint64_t)-24;

  vfs_node_t *node = kmalloc(sizeof(vfs_node_t));
  ioring_ctx_t *ctx = kmalloc(sizeof(ioring_ctx_t));
  if (!node || !ctx) {
    if (node)
      kfree(node);
    if (ctx)
      kfree(ctx);
    fd_release_reserved(t, fd);
    return (uint64_t)-12;
  }
  memset(ctx, 0, sizeof(*ctx));

  // Ring sizing: header + index/cqe arrays, page-rounded.
  uint64_t sq_ring_bytes = IORING_SQ_ARRAY_OFF + (uint64_t)sq_entries * 4;
  uint64_t cq_ring_bytes = IORING_CQ_CQES_OFF + (uint64_t)cq_entries * 16;
  uint64_t sqe_bytes = (uint64_t)sq_entries * sizeof(struct io_uring_sqe);
  ctx->sq_ring_pages = (uint32_t)((sq_ring_bytes + PAGE_SIZE - 1) / PAGE_SIZE);
  ctx->cq_ring_pages = (uint32_t)((cq_ring_bytes + PAGE_SIZE - 1) / PAGE_SIZE);
  ctx->sqe_pages = (uint32_t)((sqe_bytes + PAGE_SIZE - 1) / PAGE_SIZE);

  uint64_t sq_base = ioring_alloc_region(ctx->sq_ring_pages, &ctx->sq_ring_phys);
  uint64_t cq_base = 0, sqe_base = 0;
  if (sq_base)
    cq_base = ioring_alloc_region(ctx->cq_ring_pages, &ctx->cq_ring_phys);
  if (cq_base)
    sqe_base = ioring_alloc_region(ctx->sqe_pages, &ctx->sqe_phys);
  if (!sq_base || !cq_base || !sqe_base) {
    ioring_free_ctx_rings(ctx);
    kfree(ctx);
    kfree(node);
    fd_release_reserved(t, fd);
    return (uint64_t)-12;
  }

  ctx->magic = IORING_MAGIC;
  spinlock_init(&ctx->lock);
  wait_queue_init(&ctx->wq);
  ctx->sq_entries = sq_entries;
  ctx->cq_entries = cq_entries;
  ctx->sq_mask = sq_entries - 1;
  ctx->cq_mask = cq_entries - 1;

  ctx->sq_head = ioring_at(sq_base, IORING_SQ_HEAD_OFF);
  ctx->sq_tail = ioring_at(sq_base, IORING_SQ_TAIL_OFF);
  ctx->sq_mask_p = ioring_at(sq_base, IORING_SQ_MASK_OFF);
  ctx->sq_flags = ioring_at(sq_base, IORING_SQ_FLAGS_OFF);
  ctx->sq_dropped = ioring_at(sq_base, IORING_SQ_DROPPED_OFF);
  ctx->sq_array = ioring_at(sq_base, IORING_SQ_ARRAY_OFF);
  *ioring_at(sq_base, IORING_SQ_ENTRIES_OFF) = sq_entries;
  *ctx->sq_mask_p = ctx->sq_mask;

  ctx->cq_head = ioring_at(cq_base, IORING_CQ_HEAD_OFF);
  ctx->cq_tail = ioring_at(cq_base, IORING_CQ_TAIL_OFF);
  ctx->cq_overflow = ioring_at(cq_base, IORING_CQ_OVERFLOW_OFF);
  ctx->cqes = (volatile struct io_uring_cqe *)(cq_base + IORING_CQ_CQES_OFF);
  *ioring_at(cq_base, IORING_CQ_MASK_OFF) = ctx->cq_mask;
  *ioring_at(cq_base, IORING_CQ_ENTRIES_OFF) = cq_entries;

  ctx->sqes = (struct io_uring_sqe *)sqe_base;

  vfs_node_init(node);
  strncpy(node->name, "io_uring", sizeof(node->name) - 1);
  node->flags = FS_FILE;
  node->device = ctx;
  node->poll = ioring_poll;
  node->mmap = ioring_mmap;
  node->close = ioring_close;
  node->wait_queue = &ctx->wq;
  ctx->node = node;

  spinlock_acquire(&t->files->lock);
  t->fds[fd] = node;
  t->fd_offsets[fd] = 0;
  t->fd_flags[fd] = O_RDWR;
  fd_path_set(t, fd, "anon_inode:[io_uring]");
  spinlock_release(&t->files->lock);

  // Report back offsets and empty feature set.
  p.sq_entries = sq_entries;
  p.cq_entries = cq_entries;
  p.features = 0;
  p.sq_off.head = IORING_SQ_HEAD_OFF;
  p.sq_off.tail = IORING_SQ_TAIL_OFF;
  p.sq_off.ring_mask = IORING_SQ_MASK_OFF;
  p.sq_off.ring_entries = IORING_SQ_ENTRIES_OFF;
  p.sq_off.flags = IORING_SQ_FLAGS_OFF;
  p.sq_off.dropped = IORING_SQ_DROPPED_OFF;
  p.sq_off.array = IORING_SQ_ARRAY_OFF;
  p.sq_off.resv1 = 0;
  p.sq_off.user_addr = 0;
  p.cq_off.head = IORING_CQ_HEAD_OFF;
  p.cq_off.tail = IORING_CQ_TAIL_OFF;
  p.cq_off.ring_mask = IORING_CQ_MASK_OFF;
  p.cq_off.ring_entries = IORING_CQ_ENTRIES_OFF;
  p.cq_off.overflow = IORING_CQ_OVERFLOW_OFF;
  p.cq_off.cqes = IORING_CQ_CQES_OFF;
  p.cq_off.flags = IORING_CQ_FLAGS_OFF;
  p.cq_off.resv1 = 0;
  p.cq_off.user_addr = 0;
  if (copy_to_user((void *)params_ptr, &p, sizeof(p)) != 0) {
    // The ring is live but the caller cannot learn its offsets; tear it
    // down rather than leak an unusable descriptor.
    spinlock_acquire(&t->files->lock);
    t->fds[fd] = NULL;
    t->fd_offsets[fd] = 0;
    t->fd_flags[fd] = 0;
    fd_path_clear(t, fd);
    if (fd < (int)t->files->next_fd)
      t->files->next_fd = (uint32_t)fd;
    spinlock_release(&t->files->lock);
    node->device = NULL;
    ioring_free_ctx_rings(ctx);
    kfree(ctx);
    kfree(node);
    return (uint64_t)-14;
  }
  return (uint64_t)fd;
}

struct ioring_batch {
  struct io_uring_sqe *sqes;
  int32_t *results;
  uint32_t count;
  uint64_t ticket;
};

static uint64_t sys_io_uring_enter(uint64_t fd, uint64_t to_submit,
                                   uint64_t min_complete, uint64_t flags,
                                   uint64_t sig_ptr, uint64_t a5) {
  (void)sig_ptr;
  (void)a5;
  struct thread *t = sched_get_current();
  if (!t)
    return (uint64_t)-1;
  if (flags & ~(uint64_t)(IORING_ENTER_GETEVENTS | IORING_ENTER_SQ_WAKEUP |
                          IORING_ENTER_SQ_WAIT)) {
    if (flags & IORING_ENTER_EXT_ARG)
      return (uint64_t)-22;
    return (uint64_t)-22;
  }
  if (to_submit > IORING_MAX_ENTRIES)
    return (uint64_t)-22;

  vfs_node_t *node __attribute__((cleanup(fd_node_put))) = fd_node_get(t, fd);
  if (!node)
    return (uint64_t)-9;
  ioring_ctx_t *ctx = (ioring_ctx_t *)node->device;
  if (!ctx || ctx->magic != IORING_MAGIC)
    return (uint64_t)-22;

  // Claim phase: copy newly submitted SQEs under the lock.
  struct ioring_batch batch = {0};
  spinlock_acquire(&ctx->lock);
  uint32_t tail = __atomic_load_n((uint32_t *)ctx->sq_tail, __ATOMIC_ACQUIRE);
  uint32_t avail = tail - ctx->khead;
  uint32_t n = avail < to_submit ? avail : (uint32_t)to_submit;
  if (n > 0) {
    batch.sqes = kmalloc(n * sizeof(struct io_uring_sqe));
    batch.results = kmalloc(n * sizeof(int32_t));
    if (!batch.sqes || !batch.results) {
      if (batch.sqes)
        kfree(batch.sqes);
      if (batch.results)
        kfree(batch.results);
      spinlock_release(&ctx->lock);
      return (uint64_t)-12;
    }
    for (uint32_t i = 0; i < n; i++) {
      uint32_t idx = (ctx->khead + i) & ctx->sq_mask;
      uint32_t sq_idx = ctx->sq_array[idx];
      if (sq_idx >= ctx->sq_entries) {
        // Invalid index: consume the slot and fail this entry, keep rest.
        memset(&batch.sqes[i], 0, sizeof(batch.sqes[i]));
        batch.sqes[i].opcode = 0xFF; // marker: result already stashed
        batch.results[i] = -22;
      } else {
        batch.sqes[i] = ctx->sqes[sq_idx];
      }
    }
    ctx->khead += n;
    __atomic_store_n((uint32_t *)ctx->sq_head, ctx->khead, __ATOMIC_RELEASE);
    batch.ticket = ctx->seq_claim++;
  } else {
    batch.ticket = ctx->seq_claim++;
  }
  batch.count = n;
  spinlock_release(&ctx->lock);

  // Execute phase: no locks held (VFS ops may block).
  for (uint32_t i = 0; i < n; i++) {
    if (batch.sqes[i].opcode == 0xFF)
      continue; // result already stashed
    if (batch.sqes[i].opcode > IORING_OP_LAST) {
      batch.results[i] = -22;
      continue;
    }
    batch.results[i] = ioring_execute(t, ctx, &batch.sqes[i]);
  }

  // Complete phase: publish CQEs in claim order. Every claim (even an
  // empty one) passes through the ticket gate so sequence numbers only
  // ever move forward.
  while (__atomic_load_n(&ctx->seq_complete, __ATOMIC_ACQUIRE) != batch.ticket)
    sched_yield();
  if (n > 0) {
    spinlock_acquire(&ctx->lock);
    for (uint32_t i = 0; i < n; i++) {
      uint32_t head =
          __atomic_load_n((uint32_t *)ctx->cq_head, __ATOMIC_ACQUIRE);
      uint32_t ctail =
          __atomic_load_n((uint32_t *)ctx->cq_tail, __ATOMIC_RELAXED);
      if (ctail - head >= ctx->cq_entries) {
        // Userspace is not draining: count overflow like Linux
        // (IORING_FEAT_NODROP is not advertised) and overwrite the
        // oldest slot.
        __atomic_fetch_add((uint32_t *)ctx->cq_overflow, 1, __ATOMIC_RELAXED);
      }
      uint32_t slot = ctail & ctx->cq_mask;
      ctx->cqes[slot].user_data = batch.sqes[i].user_data;
      ctx->cqes[slot].res = batch.results[i];
      ctx->cqes[slot].flags = 0;
      __atomic_store_n((uint32_t *)ctx->cq_tail, ctail + 1, __ATOMIC_RELEASE);
    }
    spinlock_release(&ctx->lock);
    __atomic_store_n(&ctx->seq_complete, batch.ticket + 1, __ATOMIC_RELEASE);
    wait_queue_wake_all(&ctx->wq);
    if (ctx->node)
      epoll_notify_event(ctx->node, POLLIN | POLLRDNORM);
    // Signal the registered eventfd, if any.
    if (ctx->eventfd_node) {
      uint64_t one = 1;
      vfs_write(ctx->eventfd_node, 0, 8, (uint8_t *)&one);
    }
    kfree(batch.sqes);
    kfree(batch.results);
  }
  __atomic_store_n(&ctx->seq_complete, batch.ticket + 1, __ATOMIC_RELEASE);

  // min_complete: everything submitted above already completed
  // synchronously, so any requested wait is trivially satisfied.
  (void)min_complete;
  return n;
}

static uint64_t sys_io_uring_register(uint64_t fd, uint64_t opcode,
                                      uint64_t arg, uint64_t nr_args,
                                      uint64_t a4, uint64_t a5) {
  (void)a4;
  (void)a5;
  struct thread *t = sched_get_current();
  if (!t)
    return (uint64_t)-1;
  vfs_node_t *node __attribute__((cleanup(fd_node_put))) = fd_node_get(t, fd);
  if (!node)
    return (uint64_t)-9;
  ioring_ctx_t *ctx = (ioring_ctx_t *)node->device;
  if (!ctx || ctx->magic != IORING_MAGIC)
    return (uint64_t)-22;

  switch (opcode) {
  case IORING_REGISTER_BUFFERS: {
    if (!nr_args || nr_args > 1024 || !arg ||
        !vmm_is_user_addr_range_valid(arg, nr_args * sizeof(struct user_iovec)))
      return (uint64_t)-22;
    struct user_iovec *bufs = kmalloc(nr_args * sizeof(struct user_iovec));
    if (!bufs)
      return (uint64_t)-12;
    if (copy_from_user(bufs, (const void *)arg,
                       nr_args * sizeof(struct user_iovec)) != 0) {
      kfree(bufs);
      return (uint64_t)-14;
    }
    spinlock_acquire(&ctx->lock);
    if (ctx->reg_bufs)
      kfree(ctx->reg_bufs);
    ctx->reg_bufs = bufs;
    ctx->reg_buf_count = (uint32_t)nr_args;
    spinlock_release(&ctx->lock);
    return 0;
  }
  case IORING_UNREGISTER_BUFFERS: {
    spinlock_acquire(&ctx->lock);
    if (ctx->reg_bufs)
      kfree(ctx->reg_bufs);
    ctx->reg_bufs = NULL;
    ctx->reg_buf_count = 0;
    spinlock_release(&ctx->lock);
    return 0;
  }
  case IORING_REGISTER_FILES: {
    if (!nr_args || nr_args > 1024 || !arg ||
        !vmm_is_user_addr_range_valid(arg, nr_args * sizeof(int32_t)))
      return (uint64_t)-22;
    int32_t *fds = kmalloc(nr_args * sizeof(int32_t));
    vfs_node_t **nodes = kmalloc(nr_args * sizeof(vfs_node_t *));
    if (!fds || !nodes) {
      if (fds)
        kfree(fds);
      if (nodes)
        kfree(nodes);
      return (uint64_t)-12;
    }
    if (copy_from_user(fds, (const void *)arg, nr_args * sizeof(int32_t)) != 0) {
      kfree(fds);
      kfree(nodes);
      return (uint64_t)-14;
    }
    for (uint64_t i = 0; i < nr_args; i++) {
      if (fds[i] < 0) {
        nodes[i] = NULL; // sparse slot, like Linux
        continue;
      }
      nodes[i] = fd_node_get(t, (uint64_t)fds[i]);
      if (!nodes[i]) {
        for (uint64_t j = 0; j < i; j++)
          if (nodes[j])
            vfs_close(nodes[j]);
        kfree(fds);
        kfree(nodes);
        return (uint64_t)-9;
      }
    }
    kfree(fds);
    spinlock_acquire(&ctx->lock);
    for (uint32_t i = 0; i < ctx->reg_file_count; i++)
      if (ctx->reg_files[i])
        vfs_close(ctx->reg_files[i]);
    if (ctx->reg_files)
      kfree(ctx->reg_files);
    ctx->reg_files = nodes;
    ctx->reg_file_count = (uint32_t)nr_args;
    spinlock_release(&ctx->lock);
    return 0;
  }
  case IORING_UNREGISTER_FILES: {
    spinlock_acquire(&ctx->lock);
    for (uint32_t i = 0; i < ctx->reg_file_count; i++)
      if (ctx->reg_files[i])
        vfs_close(ctx->reg_files[i]);
    if (ctx->reg_files)
      kfree(ctx->reg_files);
    ctx->reg_files = NULL;
    ctx->reg_file_count = 0;
    spinlock_release(&ctx->lock);
    return 0;
  }
  case IORING_REGISTER_FILES_UPDATE: {
    if (!arg || !nr_args ||
        !vmm_is_user_addr_range_valid(arg, sizeof(struct io_uring_files_update)))
      return (uint64_t)-14;
    struct io_uring_files_update upd;
    if (copy_from_user(&upd, (const void *)arg, sizeof(upd)) != 0)
      return (uint64_t)-14;
    if (!upd.fds || nr_args > 1024 ||
        !vmm_is_user_addr_range_valid(upd.fds, nr_args * sizeof(int32_t)))
      return (uint64_t)-22;
    spinlock_acquire(&ctx->lock);
    if (upd.offset + nr_args > ctx->reg_file_count) {
      spinlock_release(&ctx->lock);
      return (uint64_t)-22;
    }
    spinlock_release(&ctx->lock);
    for (uint64_t i = 0; i < nr_args; i++) {
      int32_t newfd;
      if (copy_from_user(&newfd, (const void *)(upd.fds + i * sizeof(newfd)),
                         sizeof(newfd)) != 0)
        return (uint64_t)-14;
      vfs_node_t *nn = (newfd < 0) ? NULL : fd_node_get(t, (uint64_t)newfd);
      if (newfd >= 0 && !nn)
        return (uint64_t)-9;
      spinlock_acquire(&ctx->lock);
      if (upd.offset + i < ctx->reg_file_count) {
        if (ctx->reg_files[upd.offset + i])
          vfs_close(ctx->reg_files[upd.offset + i]);
        ctx->reg_files[upd.offset + i] = nn;
      } else if (nn) {
        vfs_close(nn);
      }
      spinlock_release(&ctx->lock);
    }
    return 0;
  }
  case IORING_REGISTER_EVENTFD:
  case IORING_REGISTER_EVENTFD_ASYNC: {
    if (!arg || !vmm_is_user_addr_range_valid(arg, sizeof(int)))
      return (uint64_t)-14;
    int evfd;
    if (copy_from_user(&evfd, (const void *)arg, sizeof(evfd)) != 0)
      return (uint64_t)-14;
    vfs_node_t *ev = fd_node_get(t, (uint64_t)(uint32_t)evfd);
    if (!ev)
      return (uint64_t)-9;
    spinlock_acquire(&ctx->lock);
    if (ctx->eventfd_node)
      vfs_close(ctx->eventfd_node);
    ctx->eventfd_node = ev;
    spinlock_release(&ctx->lock);
    return 0;
  }
  case IORING_UNREGISTER_EVENTFD: {
    spinlock_acquire(&ctx->lock);
    if (ctx->eventfd_node)
      vfs_close(ctx->eventfd_node);
    ctx->eventfd_node = NULL;
    spinlock_release(&ctx->lock);
    return 0;
  }
  case IORING_REGISTER_PROBE: {
    if (!arg || !vmm_is_user_addr_range_valid(arg, sizeof(struct io_uring_probe)))
      return (uint64_t)-14;
    struct io_uring_probe hdr;
    if (copy_from_user(&hdr, (const void *)arg, sizeof(hdr)) != 0)
      return (uint64_t)-14;
    // Ops this implementation executes for real.
    static const uint8_t supported[] = {
        IORING_OP_NOP,         IORING_OP_READV,   IORING_OP_WRITEV,
        IORING_OP_FSYNC,       IORING_OP_READ_FIXED, IORING_OP_WRITE_FIXED,
        IORING_OP_POLL_ADD,    IORING_OP_POLL_REMOVE, IORING_OP_SYNC_FILE_RANGE,
        IORING_OP_OPENAT,      IORING_OP_CLOSE,   IORING_OP_READ,
        IORING_OP_WRITE,
    };
    // Linux probe semantics: ops[i] describes opcode i; supported ones
    // carry IO_URING_OP_SUPPORTED. last_op is the highest opcode known.
    uint8_t want = hdr.ops_len;
    if (want > IORING_OP_LAST)
      want = IORING_OP_LAST;
    for (uint8_t i = 0; i < want; i++) {
      struct io_uring_probe_op op = {.op = i};
      for (size_t k = 0; k < sizeof(supported); k++)
        if (supported[k] == i) {
          op.flags = IO_URING_OP_SUPPORTED;
          break;
        }
      if (copy_to_user((void *)(arg + sizeof(hdr) + i * sizeof(op)), &op,
                       sizeof(op)) != 0)
        return (uint64_t)-14;
    }
    hdr.last_op = IORING_OP_LAST - 1;
    if (copy_to_user((void *)arg, &hdr, sizeof(hdr)) != 0)
      return (uint64_t)-14;
    return 0;
  }
  case IORING_REGISTER_ENABLE_RINGS:
    spinlock_acquire(&ctx->lock);
    ctx->rings_disabled = false;
    spinlock_release(&ctx->lock);
    return 0;
  default:
    return (uint64_t)-22;
  }
}

void syscall_register_io_uring(void) {
  syscall_register(SYS_IO_URING_SETUP, sys_io_uring_setup);
  syscall_register(SYS_IO_URING_ENTER, sys_io_uring_enter);
  syscall_register(SYS_IO_URING_REGISTER, sys_io_uring_register);
}
