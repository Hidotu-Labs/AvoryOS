// sys_aio.c — Async / event I/O syscalls:
//   eventfd2, timerfd_create/settime/gettime, pipe, pipe2,
//   inotify_init, inotify_init1, inotify_add_watch,
//   memfd_create
#include "sys_io_shared.h"
#include "../apic/lapic_timer.h"
#include "../console/klog.h"
#include "../fs/ramfs.h"
#include "../fs/vfs.h"
#include "../lib/string.h"
#include "../mm/heap.h"
#include "../mm/pmm.h"
#include "../mm/vmm.h"
#include "../sched/sched.h"
#include "../sched/wait.h"
#include "syscall.h"
#include "../socket/epoll.h"
#include <stdint.h>

// ---------------------------------------------------------------------------
// inotify globals (shared via sys_io_shared.h extern declarations)
// ---------------------------------------------------------------------------

inotify_instance_t inotify_instances[MAX_INOTIFY_INSTANCES];
uint32_t next_instance_id = 1;
uint32_t next_watch_id    = 1;

// ---------------------------------------------------------------------------
// eventfd2
// ---------------------------------------------------------------------------

#define EFD_SEMAPHORE 1
#define EFD_CLOEXEC   02000000
#define EFD_NONBLOCK  04000

typedef struct {
    uint64_t    counter;
    uint32_t    flags;
    wait_queue_t wq;
    spinlock_t  lock;
} eventfd_ctx_t;

static uint32_t eventfd_read(vfs_node_t *node, uint32_t offset, uint32_t size,
                              uint8_t *buffer) {
    (void)offset;
    if (size < 8) return (uint32_t)-22;
    eventfd_ctx_t *ctx = (eventfd_ctx_t *)node->device;
    if (!ctx) return (uint32_t)-22;

    struct thread *t = sched_get_current();
    int fd = -1;
    for (int i = 0; i < MAX_FDS; i++) {
        if (t->fds[i] == node) { fd = i; break; }
    }

    spinlock_acquire(&ctx->lock);
    while (ctx->counter == 0) {
        if (fd != -1 && (t->fd_flags[fd] & 0x800)) {
            spinlock_release(&ctx->lock);
            return (uint32_t)-11;
        }
        wait_queue_entry_t entry = {.thread = t, .next = NULL};
        wait_queue_add(&ctx->wq, &entry);
        t->state = THREAD_BLOCKED;
        spinlock_release(&ctx->lock);
        sched_yield();
        t->state = THREAD_RUNNING;
        wait_queue_remove(&ctx->wq, &entry);
        spinlock_acquire(&ctx->lock);
    }
    uint64_t val;
    if (ctx->flags & EFD_SEMAPHORE) { val = 1; ctx->counter -= 1; }
    else { val = ctx->counter; ctx->counter = 0; }
    spinlock_release(&ctx->lock);
    memcpy(buffer, &val, 8);
    return 8;
}

static uint32_t eventfd_write(vfs_node_t *node, uint32_t offset, uint32_t size,
                               uint8_t *buffer) {
    (void)offset;
    if (size < 8) return (uint32_t)-22;
    eventfd_ctx_t *ctx = (eventfd_ctx_t *)node->device;
    if (!ctx) return (uint32_t)-22;

    uint64_t val;
    memcpy(&val, buffer, 8);
    if (val == 0) return (uint32_t)-22;

    spinlock_acquire(&ctx->lock);
    ctx->counter += val;
    spinlock_release(&ctx->lock);

    wait_queue_wake_all(&ctx->wq);
    extern void epoll_notify_event(struct vfs_node *node, uint32_t events);
    epoll_notify_event(node, 0x0001);
    return 8;
}

static int eventfd_poll(vfs_node_t *node, int events) {
    eventfd_ctx_t *ctx = (eventfd_ctx_t *)node->device;
    if (!ctx) return 0;
    int revents = 0;
    spinlock_acquire(&ctx->lock);
    if (ctx->counter > 0)                       revents |= POLLIN;
    if (ctx->counter < 0xfffffffffffffffeULL)    revents |= POLLOUT;
    spinlock_release(&ctx->lock);
    return revents & events;
}

static void eventfd_close(vfs_node_t *node) {
    if (node && node->device) {
        eventfd_ctx_t *ctx = (eventfd_ctx_t *)node->device;
        /* Detach every waiter before the context goes away: the waker clears
         * each entry's queue back-pointer, so a thread that was parked here
         * (or is cleaned up later at exit) never dereferences freed memory. */
        wait_queue_wake_all(&ctx->wq);
        kfree(ctx);
        node->device = NULL;
    }
}

static uint64_t sys_eventfd2(uint64_t initval, uint64_t flags, uint64_t a2,
                              uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a2; (void)a3; (void)a4; (void)a5;
    struct thread *t = sched_get_current();
    if (!t) return (uint64_t)-1;
    /* EFD_CLOEXEC is a descriptor flag, not a status flag (see pipe2). */
    if (flags & ~(uint64_t)(EFD_CLOEXEC | EFD_NONBLOCK | EFD_SEMAPHORE))
        return (uint64_t)-22; // EINVAL

    int fd = alloc_fd(t);
    if (fd < 0) return (uint64_t)-24;

    vfs_node_t *node = kmalloc(sizeof(vfs_node_t));
    if (!node) return (uint64_t)-12;
    vfs_node_init(node);

    eventfd_ctx_t *ctx = kmalloc(sizeof(eventfd_ctx_t));
    if (!ctx) { kfree(node); return (uint64_t)-12; }
    memset(ctx, 0, sizeof(eventfd_ctx_t));
    ctx->counter = initval;
    ctx->flags   = flags;
    spinlock_init(&ctx->lock);
    wait_queue_init(&ctx->wq);

    node->flags      = FS_FILE;
    node->device     = ctx;
    node->read       = eventfd_read;
    node->write      = eventfd_write;
    node->poll       = eventfd_poll;
    node->close      = eventfd_close;
    node->wait_queue = &ctx->wq;

    t->fds[fd]        = node;
    t->fd_offsets[fd] = 0;
    t->fd_flags[fd]   = (flags & EFD_NONBLOCK ? O_NONBLOCK : 0) |
                        (flags & EFD_CLOEXEC ? FD_FLAGS_CLOEXEC_BIT : 0);
    return fd;
}

// ---------------------------------------------------------------------------
// timerfd
// ---------------------------------------------------------------------------

#define TFD_NONBLOCK 04000
#define TFD_CLOEXEC  02000000

typedef struct {
    uint32_t     clockid;
    uint32_t     flags;
    uint64_t     interval_sec;
    uint64_t     interval_nsec;
    uint64_t     expire_ms;
    uint64_t     expirations;
    spinlock_t   lock;
    wait_queue_t wq;
    struct vfs_node *node;
} timerfd_ctx_t;

#define TIMERFD_MAX 64
static timerfd_ctx_t *timerfd_table[TIMERFD_MAX];
static spinlock_t timerfd_table_lock = SPINLOCK_INIT;
static volatile uint32_t timerfd_active_count = 0;

static void timerfd_register(timerfd_ctx_t *ctx) {
    spinlock_acquire(&timerfd_table_lock);
    for (int i = 0; i < TIMERFD_MAX; i++) {
        if (!timerfd_table[i]) {
            timerfd_table[i] = ctx;
            __atomic_fetch_add(&timerfd_active_count, 1, __ATOMIC_RELAXED);
            break;
        }
    }
    spinlock_release(&timerfd_table_lock);
}

static void timerfd_unregister(timerfd_ctx_t *ctx) {
    spinlock_acquire(&timerfd_table_lock);
    for (int i = 0; i < TIMERFD_MAX; i++) {
        if (timerfd_table[i] == ctx) {
            timerfd_table[i] = NULL;
            if (timerfd_active_count)
                __atomic_fetch_sub(&timerfd_active_count, 1, __ATOMIC_RELAXED);
            break;
        }
    }
    spinlock_release(&timerfd_table_lock);
}

void timerfd_tick(void) {
    if (__atomic_load_n(&timerfd_active_count, __ATOMIC_RELAXED) == 0)
        return;

    extern uint64_t lapic_timer_get_ms(void);
    extern void epoll_notify_event(struct vfs_node *node, uint32_t events);
    uint64_t now = lapic_timer_get_ms();

    if (!spinlock_try_acquire(&timerfd_table_lock)) return;

    for (int i = 0; i < TIMERFD_MAX; i++) {
        timerfd_ctx_t *ctx = timerfd_table[i];
        if (!ctx || ctx->expire_ms == 0) continue;
        if (now >= ctx->expire_ms) {
            if (!spinlock_try_acquire(&ctx->lock)) continue;
            ctx->expirations++;
            if (ctx->interval_sec || ctx->interval_nsec) {
                uint64_t iv = ctx->interval_sec * 1000 + ctx->interval_nsec / 1000000;
                if (iv == 0) iv = 1;
                ctx->expire_ms = now + iv;
                lapic_timer_rearm_if_earlier(ctx->expire_ms);
            } else {
                ctx->expire_ms = 0;
            }
            if (ctx->node) {
                vfs_node_t *node = ctx->node;
                spinlock_release(&ctx->lock);
                epoll_notify_event(node, 0x0001);
                wait_queue_wake_all(&ctx->wq);
            } else {
                spinlock_release(&ctx->lock);
            }
        }
    }
    spinlock_release(&timerfd_table_lock);
}

static uint32_t timerfd_read(vfs_node_t *node, uint32_t offset, uint32_t size,
                              uint8_t *buffer) {
    (void)offset;
    if (size < 8) return (uint32_t)-22;
    timerfd_ctx_t *ctx = (timerfd_ctx_t *)node->device;
    if (!ctx) return (uint32_t)-22;

    spinlock_acquire(&ctx->lock);
    while (ctx->expirations == 0) {
        struct thread *t = sched_get_current();
        int fd = -1;
        for (int i = 0; i < MAX_FDS; i++) {
            if (t->fds[i] == node) { fd = i; break; }
        }
        if (fd != -1 && (t->fd_flags[fd] & 0x800)) {
            spinlock_release(&ctx->lock);
            return (uint32_t)-11;
        }
        wait_queue_entry_t entry = {.thread = t, .next = NULL};
        wait_queue_add(&ctx->wq, &entry);
        t->state = THREAD_BLOCKED;
        spinlock_release(&ctx->lock);
        sched_yield();
        t->state = THREAD_RUNNING;
        wait_queue_remove(&ctx->wq, &entry);
        spinlock_acquire(&ctx->lock);
    }
    uint64_t val = ctx->expirations;
    ctx->expirations = 0;
    spinlock_release(&ctx->lock);
    memcpy(buffer, &val, 8);
    return 8;
}

static int timerfd_poll(vfs_node_t *node, int events) {
    timerfd_ctx_t *ctx = (timerfd_ctx_t *)node->device;
    if (!ctx) return 0;
    int revents = 0;
    spinlock_acquire(&ctx->lock);
    if (ctx->expirations > 0) revents |= POLLIN;
    spinlock_release(&ctx->lock);
    return revents & events;
}

static void timerfd_close(vfs_node_t *node) {
    if (node && node->device) {
        timerfd_ctx_t *ctx = (timerfd_ctx_t *)node->device;
        timerfd_unregister(ctx);
        wait_queue_wake_all(&ctx->wq);
        ctx->node = NULL;
        kfree(ctx);
        node->device = NULL;
    }
}

static uint64_t sys_timerfd_create(uint64_t clockid, uint64_t flags,
                                    uint64_t a2, uint64_t a3, uint64_t a4,
                                    uint64_t a5) {
    (void)a2; (void)a3; (void)a4; (void)a5;
    struct thread *t = sched_get_current();
    if (!t) return (uint64_t)-1;
    if (clockid != 0 && clockid != 1 && clockid != 7) return (uint64_t)-22;
    if (flags & ~(uint64_t)(TFD_NONBLOCK | TFD_CLOEXEC))  return (uint64_t)-22;

    int fd = alloc_fd(t);
    if (fd < 0) return (uint64_t)-24;

    timerfd_ctx_t *ctx = kmalloc(sizeof(timerfd_ctx_t));
    if (!ctx) return (uint64_t)-12;
    memset(ctx, 0, sizeof(timerfd_ctx_t));
    ctx->clockid = (uint32_t)clockid;
    ctx->flags   = (uint32_t)flags;
    spinlock_init(&ctx->lock);
    wait_queue_init(&ctx->wq);

    vfs_node_t *node = kmalloc(sizeof(vfs_node_t));
    if (!node) { kfree(ctx); return (uint64_t)-12; }
    vfs_node_init(node);
    node->flags      = FS_FILE;
    node->device     = ctx;
    node->read       = timerfd_read;
    node->poll       = timerfd_poll;
    node->close      = timerfd_close;
    node->wait_queue = &ctx->wq;
    ctx->node        = node;
    timerfd_register(ctx);

    t->fds[fd]        = node;
    t->fd_offsets[fd] = 0;
    uint64_t fd_flags_val = 0;
    if (flags & TFD_NONBLOCK) fd_flags_val |= O_NONBLOCK;
    if (flags & TFD_CLOEXEC)  fd_flags_val |= FD_FLAGS_CLOEXEC_BIT;
    t->fd_flags[fd] = fd_flags_val;
    return (uint64_t)fd;
}

struct itimerspec {
    uint64_t it_interval_sec;
    uint64_t it_interval_nsec;
    uint64_t it_value_sec;
    uint64_t it_value_nsec;
};

static uint64_t sys_timerfd_settime(uint64_t fd, uint64_t flags_arg,
                                     uint64_t new_value_ptr,
                                     uint64_t old_value_ptr, uint64_t a4,
                                     uint64_t a5) {
    (void)a4; (void)a5;
    struct thread *t = sched_get_current();
    if (!t || fd >= MAX_FDS || !t->fds[fd]) return (uint64_t)-9;

    vfs_node_t    *node = t->fds[fd];
    timerfd_ctx_t *ctx  = (timerfd_ctx_t *)node->device;
    if (!ctx) return (uint64_t)-22;

    if (old_value_ptr &&
        vmm_is_user_addr_range_valid(old_value_ptr, sizeof(struct itimerspec))) {
        struct itimerspec *old = (struct itimerspec *)old_value_ptr;
        old->it_interval_sec  = ctx->interval_sec;
        old->it_interval_nsec = ctx->interval_nsec;
        old->it_value_sec     = 0;
        old->it_value_nsec    = 0;
    }
    if (!new_value_ptr ||
        !vmm_is_user_addr_range_valid(new_value_ptr, sizeof(struct itimerspec)))
        return (uint64_t)-14;

    struct itimerspec *nv = (struct itimerspec *)new_value_ptr;
    spinlock_acquire(&ctx->lock);
    ctx->interval_sec  = nv->it_interval_sec;
    ctx->interval_nsec = nv->it_interval_nsec;
    ctx->expirations   = 0;

    if (nv->it_value_sec == 0 && nv->it_value_nsec == 0) {
        ctx->expire_ms = 0;
    } else {
        extern uint64_t lapic_timer_get_ms(void);
        extern uint64_t rtc_get_boot_timestamp(void);
        uint64_t lapic_now = lapic_timer_get_ms();
        uint64_t value_ms  = nv->it_value_sec * 1000 + nv->it_value_nsec / 1000000;
        if (flags_arg & 1) { // TFD_TIMER_ABSTIME
            if (ctx->clockid == 0) {
                uint64_t boot_unix_ms = rtc_get_boot_timestamp() * 1000;
                uint64_t current_realtime_ms = boot_unix_ms + lapic_now;
                if (value_ms <= current_realtime_ms)
                    ctx->expire_ms = lapic_now + 1;
                else
                    ctx->expire_ms = lapic_now + (value_ms - current_realtime_ms);
            } else {
                ctx->expire_ms = (value_ms <= lapic_now) ? lapic_now + 1 : value_ms;
            }
        } else {
            uint64_t delay_ms = value_ms ? value_ms : 1;
            ctx->expire_ms = lapic_now + delay_ms;
        }
    }
    spinlock_release(&ctx->lock);
    if (ctx->expire_ms)
        lapic_timer_rearm_if_earlier(ctx->expire_ms);
    return 0;
}

static uint64_t sys_timerfd_gettime(uint64_t fd, uint64_t curr_value_ptr,
                                     uint64_t a2, uint64_t a3, uint64_t a4,
                                     uint64_t a5) {
    (void)a2; (void)a3; (void)a4; (void)a5;
    struct thread *t = sched_get_current();
    if (!t || fd >= MAX_FDS || !t->fds[fd]) return (uint64_t)-9;

    timerfd_ctx_t *ctx = (timerfd_ctx_t *)t->fds[fd]->device;
    if (!ctx || !curr_value_ptr ||
        !vmm_is_user_addr_range_valid(curr_value_ptr, sizeof(struct itimerspec)))
        return (uint64_t)-14;

    struct itimerspec *cv = (struct itimerspec *)curr_value_ptr;
    spinlock_acquire(&ctx->lock);
    cv->it_interval_sec  = ctx->interval_sec;
    cv->it_interval_nsec = ctx->interval_nsec;
    if (ctx->expire_ms == 0) {
        cv->it_value_sec = 0; cv->it_value_nsec = 0;
    } else {
        extern uint64_t lapic_timer_get_ms(void);
        uint64_t now = lapic_timer_get_ms();
        if (now >= ctx->expire_ms) {
            cv->it_value_sec = 0; cv->it_value_nsec = 1;
        } else {
            uint64_t remaining_ms  = ctx->expire_ms - now;
            cv->it_value_sec  = remaining_ms / 1000;
            cv->it_value_nsec = (remaining_ms % 1000) * 1000000;
        }
    }
    spinlock_release(&ctx->lock);
    return 0;
}

// ---------------------------------------------------------------------------
// pipe / pipe2
// ---------------------------------------------------------------------------

typedef struct {
    ramfs_file_t ramfs;
    wait_queue_t wq;
    spinlock_t   lock;
    uint32_t length;
    uint32_t read_offset;
    bool reader_open;
    bool writer_open;
    vfs_node_t *read_node;
} pipe_ctx_t;

typedef struct {
    pipe_ctx_t *ctx;
    bool writer;
} pipe_end_t;

static uint32_t pipe_read(vfs_node_t *node, uint32_t offset, uint32_t size,
                           uint8_t *buffer) {
    (void)offset;
    pipe_end_t *end = (pipe_end_t *)node->device;
    pipe_ctx_t *ctx = end ? end->ctx : NULL;
    if (!ctx) return 0;

    spinlock_acquire(&ctx->lock);
    while (ctx->read_offset >= ctx->length) {
        struct thread *t = sched_get_current();
        int fd = -1;
        for (int i = 0; i < MAX_FDS; i++) {
            if (t->fds[i] == node) { fd = i; break; }
        }
        /* EOF takes precedence over O_NONBLOCK/EAGAIN. */
        if (!ctx->writer_open) { spinlock_release(&ctx->lock); return 0; }
        if (fd != -1 && (t->fd_flags[fd] & 0x800)) {
            klog_debugf("[PIPE] EAGAIN ctx=0x%llx fd=%d writer_open=1 read_node_refs=%u\n",
                        (unsigned long long)(uintptr_t)ctx, fd, node->refcount);
            spinlock_release(&ctx->lock);
            return (uint32_t)-11;
        }
        wait_queue_entry_t entry = {.thread = t, .next = NULL};
        wait_queue_add(&ctx->wq, &entry);
        t->state = THREAD_BLOCKED;
        spinlock_release(&ctx->lock);
        sched_yield();
        t->state = THREAD_RUNNING;
        wait_queue_remove(&ctx->wq, &entry);
        spinlock_acquire(&ctx->lock);
    }

    vfs_node_t storage = {0};
    storage.device = &ctx->ramfs;
    storage.length = ctx->length;
    int32_t ret = (int32_t)ramfs_read(&storage, ctx->read_offset, size, buffer);
    if (ret > 0) {
        ctx->read_offset += (uint32_t)ret;
        if (ctx->read_offset >= ctx->length) {
            ctx->read_offset = 0;
            ctx->length = 0;
        }
    }
    spinlock_release(&ctx->lock);
    return (uint32_t)ret;
}

static uint32_t pipe_write(vfs_node_t *node, uint32_t offset, uint32_t size,
                            uint8_t *buffer) {
    (void)offset;
    pipe_end_t *end = (pipe_end_t *)node->device;
    pipe_ctx_t *ctx = end ? end->ctx : NULL;
    if (!ctx) return 0;

    spinlock_acquire(&ctx->lock);
    if (!ctx->reader_open) { spinlock_release(&ctx->lock); return (uint32_t)-32; }
    vfs_node_t storage = {0};
    storage.device = &ctx->ramfs;
    storage.length = ctx->length;
    int32_t ret = (int32_t)ramfs_write(&storage, ctx->length, size, buffer);
    if (ret > 0) {
        ctx->length = storage.length;
        vfs_node_t *rnode = ctx->reader_open ? ctx->read_node : NULL;
        spinlock_release(&ctx->lock);
        wait_queue_wake_all(&ctx->wq);
        if (rnode)
            epoll_notify_event(rnode, 0x0001);
        return (uint32_t)ret;
    }
    spinlock_release(&ctx->lock);
    return (uint32_t)ret;
}

static int pipe_poll(vfs_node_t *node, int events) {
    pipe_end_t *end = (pipe_end_t *)node->device;
    pipe_ctx_t *ctx = end ? end->ctx : NULL;
    if (!ctx) return 0;
    int revents = 0;
    spinlock_acquire(&ctx->lock);
    if (!end->writer && ctx->length > ctx->read_offset) revents |= (POLLIN | POLLRDNORM);
    if (!end->writer && !ctx->writer_open)              revents |= POLLHUP;
    if (end->writer && !ctx->reader_open)               revents |= POLLERR;
    if (end->writer && ctx->reader_open)                revents |= (POLLOUT | POLLWRNORM);
    spinlock_release(&ctx->lock);
    /* poll(2) reports POLLERR and POLLHUP regardless of the requested mask.
     * A POLLIN-only reader must wake when the final pipe writer closes. */
    return revents & (events | POLLERR | POLLHUP | POLLNVAL);
}

static void pipe_close(vfs_node_t *node) {
    if (!node || !node->device) return;
    pipe_end_t *end = (pipe_end_t *)node->device;
    pipe_ctx_t *ctx = end->ctx;
    vfs_node_t *notify_read = NULL;
    spinlock_acquire(&ctx->lock);
    if (end->writer) {
        ctx->writer_open = false;
        if (ctx->reader_open && ctx->read_node)
            notify_read = ctx->read_node;
    } else {
        ctx->reader_open = false;
        ctx->read_node = NULL;
    }
    wait_queue_wake_all(&ctx->wq);
    bool free_ctx = !ctx->reader_open && !ctx->writer_open;
    spinlock_release(&ctx->lock);
    if (notify_read)
        epoll_notify_event(notify_read, POLLHUP);
    kfree(end);
    node->device = NULL;
    if (free_ctx) {
        ramfs_free_file_data(&ctx->ramfs);
        kfree(ctx);
    }
}

static uint64_t sys_pipe2(uint64_t pipefd_ptr, uint64_t flags, uint64_t a2,
                           uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a2; (void)a3; (void)a4; (void)a5;
    /* O_CLOEXEC is a descriptor flag and must not be confused with the status
     * flags stored in fd_flags[]; O_NONBLOCK is a status flag.  Unknown flags
     * have to be rejected rather than swallowed: Qt's forkfd probes for
     * O_NOSIGPIPE by expecting EINVAL, and a silently ignored O_CLOEXEC lets a
     * caller believe the descriptor cannot survive execve.  That is how the
     * QProcess "child started" pipe used to leak into bash: the parent's
     * poll() for the end-of-file never fired, its 30 second wait expired, and
     * Konsole reported "Could not start program '/bin/bash'" over a shell that
     * was in fact running. */
#ifndef O_ASYNC
#define O_ASYNC 0x2000
#endif
    const uint64_t PIPE_SUPPORTED_FLAGS = O_CLOEXEC | O_NONBLOCK | O_ASYNC;
    int *pipefd = (int *)pipefd_ptr;
    if (!pipefd) return (uint64_t)-14;
    if (flags & ~PIPE_SUPPORTED_FLAGS) return (uint64_t)-22; // EINVAL

    struct thread *t = sched_get_current();
    if (!t) return (uint64_t)-1;

    int fd_read = alloc_fd(t);
    if (fd_read < 0) return (uint64_t)-24;
    t->fds[fd_read] = (void *)-1;

    int fd_write = alloc_fd(t);
    if (fd_write < 0) { t->fds[fd_read] = NULL; return (uint64_t)-24; }

    pipe_ctx_t *ctx = kmalloc(sizeof(pipe_ctx_t));
    if (!ctx) {
        t->fds[fd_read] = t->fds[fd_write] = NULL;
        return (uint64_t)-12;
    }
    memset(ctx, 0, sizeof(pipe_ctx_t));
    spinlock_init(&ctx->lock);
    wait_queue_init(&ctx->wq);
    ctx->reader_open = true;
    ctx->writer_open = true;

    pipe_end_t *read_end = kmalloc(sizeof(pipe_end_t));
    pipe_end_t *write_end = kmalloc(sizeof(pipe_end_t));
    vfs_node_t *read_node = kmalloc(sizeof(vfs_node_t));
    vfs_node_t *write_node = kmalloc(sizeof(vfs_node_t));
    if (!read_end || !write_end || !read_node || !write_node) {
        if (read_end) kfree(read_end);
        if (write_end) kfree(write_end);
        if (read_node) kfree(read_node);
        if (write_node) kfree(write_node);
        kfree(ctx);
        t->fds[fd_read] = t->fds[fd_write] = NULL;
        return (uint64_t)-12;
    }
    read_end->ctx = write_end->ctx = ctx;
    read_end->writer = false;
    write_end->writer = true;
    vfs_node_init(read_node);
    vfs_node_init(write_node);
    read_node->flags = write_node->flags = FS_PIPE;
    read_node->device = read_end;
    write_node->device = write_end;
    read_node->read = pipe_read;
    write_node->write = pipe_write;
    read_node->poll = write_node->poll = pipe_poll;
    read_node->close = write_node->close = pipe_close;
    read_node->wait_queue = write_node->wait_queue = &ctx->wq;
    ctx->read_node = read_node;

    t->fds[fd_read] = read_node;
    t->fds[fd_write] = write_node;
    t->fd_offsets[fd_read]  = t->fd_offsets[fd_write]  = 0;
    /* Translate O_CLOEXEC into the descriptor flag exec_close_cloexec() looks
     * for instead of leaving it in the status flags, where it does nothing. */
    const uint64_t pipe_status = flags & ~(uint64_t)(O_ACCMODE | O_CLOEXEC);
    t->fd_flags[fd_read]  = pipe_status | O_RDONLY;
    t->fd_flags[fd_write] = pipe_status | O_WRONLY;
    if (flags & O_CLOEXEC) {
        t->fd_flags[fd_read]  |= FD_FLAGS_CLOEXEC_BIT;
        t->fd_flags[fd_write] |= FD_FLAGS_CLOEXEC_BIT;
    }
    pipefd[0] = fd_read;
    pipefd[1] = fd_write;
    return 0;
}

static uint64_t sys_pipe(uint64_t pipefd_ptr, uint64_t a1, uint64_t a2,
                          uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a1;
    return sys_pipe2(pipefd_ptr, 0, a2, a3, a4, a5);
}

// ---------------------------------------------------------------------------
// inotify
// ---------------------------------------------------------------------------

static void inotify_close(vfs_node_t *node) {
    if (!node || !node->device) return;
    inotify_instance_t *instance = (inotify_instance_t *)node->device;
    memset(instance, 0, sizeof(*instance));
    node->device = NULL;
}

static uint64_t sys_inotify_init(uint64_t a1, uint64_t a2, uint64_t a3,
                                  uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    struct thread *t = sched_get_current();
    if (!t) return (uint64_t)-1;

    inotify_instance_t *instance = NULL;
    for (int i = 0; i < MAX_INOTIFY_INSTANCES; i++) {
        if (inotify_instances[i].instance_id == 0) {
            instance = &inotify_instances[i]; break;
        }
    }
    if (!instance) return (uint64_t)-23;

    instance->instance_id = next_instance_id++;
    instance->num_watches = 0;
    instance->queue_head  = instance->queue_tail = 0;

    int fd = alloc_fd(t);
    if (fd < 0) return (uint64_t)-24;

    vfs_node_t *node = kmalloc(sizeof(vfs_node_t));
    if (!node) return (uint64_t)-12;
    vfs_node_init(node);
    node->flags = FS_CHARDEV;
    node->mask  = 0600;
    node->impl  = instance->instance_id;
    node->device = instance;
    node->close = inotify_close;
    node->name[0] = '\0';
    strcat(node->name, "inotify");

    t->fds[fd]        = node;
    t->fd_offsets[fd] = 0;
    fd_path_set(t, fd, "inotify");

    klog_debug_puts("[INOTIFY_INIT] Created instance ");
    klog_debug_uint64(instance->instance_id);
    klog_debug_puts(" with fd="); klog_debug_uint64(fd); klog_debug_puts("\n");
    return fd;
}

static uint64_t sys_inotify_init1(uint64_t flags, uint64_t a2, uint64_t a3,
                                   uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    /* IN_NONBLOCK and IN_CLOEXEC are defined to be O_NONBLOCK and O_CLOEXEC. */
    if (flags & ~(uint64_t)(O_CLOEXEC | O_NONBLOCK))
        return (uint64_t)-22; // EINVAL
    uint64_t fd = sys_inotify_init(0, 0, 0, 0, 0, 0);
    if ((int64_t)fd >= 0 && fd < MAX_FDS) {
        struct thread *t = sched_get_current();
        if (t && (flags & O_NONBLOCK)) t->fd_flags[fd] |= O_NONBLOCK;
        if (t && (flags & O_CLOEXEC))
            t->fd_flags[fd] |= FD_FLAGS_CLOEXEC_BIT;
    }
    return fd;
}

static uint64_t sys_inotify_add_watch(uint64_t fd, uint64_t pathname,
                                       uint64_t mask, uint64_t a4,
                                       uint64_t a5, uint64_t a6) {
    (void)a4; (void)a5; (void)a6;
    struct thread *t = sched_get_current();
    if (!t) return (uint64_t)-1;
    if (!pathname || !is_user_ptr(pathname)) return (uint64_t)-14;
    if (fd >= MAX_FDS || !t->fds[fd]) return (uint64_t)-9;

    vfs_node_t *node = t->fds[fd];
    if (strcmp(node->name, "inotify") != 0) return (uint64_t)-22;

    uint32_t instance_id = node->impl;
    inotify_instance_t *instance = NULL;
    for (int i = 0; i < MAX_INOTIFY_INSTANCES; i++) {
        if (inotify_instances[i].instance_id == instance_id) {
            instance = &inotify_instances[i]; break;
        }
    }
    if (!instance) return (uint64_t)-22;
    if (instance->num_watches >= MAX_INOTIFY_WATCHES) return (uint64_t)-23;

    const char *path = (const char *)pathname;
    for (uint32_t i = 0; i < instance->num_watches; i++) {
        if (strcmp(instance->watches[i].path, path) == 0) {
            instance->watches[i].mask = (uint32_t)mask;
            klog_debug_puts("[INOTIFY_ADD_WATCH] Updated watch for path: ");
            klog_debug_puts(path); klog_debug_puts("\n");
            return instance->watches[i].wd;
        }
    }

    inotify_watch_t *watch = &instance->watches[instance->num_watches];
    watch->wd         = next_watch_id++;
    watch->mask       = (uint32_t)mask;
    watch->event_mask = 0;
    strncpy(watch->path, path, sizeof(watch->path) - 1);
    watch->path[sizeof(watch->path) - 1] = '\0';
    instance->num_watches++;

    klog_debug_puts("[INOTIFY_ADD_WATCH] Added watch wd="); klog_debug_uint64(watch->wd);
    klog_debug_puts(" for path: "); klog_debug_puts(path);
    klog_debug_puts(" in instance "); klog_debug_uint64(instance_id); klog_debug_puts("\n");
    return watch->wd;
}

// ---------------------------------------------------------------------------
// inotify_rm_watch
// ---------------------------------------------------------------------------

static uint64_t sys_inotify_rm_watch(uint64_t fd_val, uint64_t wd_val,
                                      uint64_t a3, uint64_t a4,
                                      uint64_t a5, uint64_t a6) {
    (void)a3; (void)a4; (void)a5; (void)a6;
    struct thread *t = sched_get_current();
    if (!t) return (uint64_t)-1;

    int fd = (int)fd_val;
    if (fd < 0 || fd >= MAX_FDS || !t->fds[fd]) return (uint64_t)-9; // EBADF

    vfs_node_t *node = t->fds[fd];
    if (strcmp(node->name, "inotify") != 0) return (uint64_t)-22; // EINVAL

    uint32_t instance_id = node->impl;
    inotify_instance_t *instance = NULL;
    for (int i = 0; i < MAX_INOTIFY_INSTANCES; i++) {
        if (inotify_instances[i].instance_id == instance_id) {
            instance = &inotify_instances[i]; break;
        }
    }
    if (!instance) return (uint64_t)-22; // EINVAL

    uint32_t target_wd = (uint32_t)wd_val;
    int found_idx = -1;
    for (uint32_t i = 0; i < instance->num_watches; i++) {
        if (instance->watches[i].wd == target_wd) {
            found_idx = (int)i;
            break;
        }
    }
    if (found_idx < 0) return (uint64_t)-22; // EINVAL

    for (uint32_t i = (uint32_t)found_idx; i + 1 < instance->num_watches; i++)
        instance->watches[i] = instance->watches[i + 1];
    instance->num_watches--;

    klog_debug_puts("[INOTIFY_RM_WATCH] Removed watch wd=");
    klog_debug_uint64(target_wd);
    klog_debug_puts(" in instance ");
    klog_debug_uint64(instance_id);
    klog_debug_puts("\n");
    return 0;
}

// ---------------------------------------------------------------------------
// memfd_create
// ---------------------------------------------------------------------------

#define MFD_CLOEXEC      0x0001U
#define MFD_ALLOW_SEALING 0x0002U

static void memfd_close(vfs_node_t *node) {
    if (!node || !node->device) return;

    ramfs_file_t *file = (ramfs_file_t *)node->device;
    ramfs_free_file_data(file);
    kfree(file);
    node->device = NULL;
}

static uint64_t memfd_mmap(vfs_node_t *node, uint64_t addr, uint64_t length,
                            uint64_t prot, uint64_t flags, uint64_t offset) {
    (void)flags;
    if (!node || !node->device) return (uint64_t)-1;

    ramfs_file_t *file = (ramfs_file_t *)node->device;
    uint64_t hhdm      = pmm_get_hhdm_offset();
    uint64_t needed    = offset + length;

    if (needed > file->capacity || !file->data || !file->data_is_pmm || ((uint64_t)file->data & 0xFFF)) {
        uint32_t new_cap = (uint32_t)needed;
        if (new_cap < 4096) new_cap = 4096;
        new_cap = (new_cap + 0xFFF) & ~0xFFFU;

        uint32_t old_cap = file->capacity;
        uint8_t old_is_pmm = file->data_is_pmm;
        void *phys = pmm_alloc_pages(new_cap / PAGE_SIZE);
        if (!phys) return (uint64_t)-12;
        uint8_t *new_data = (uint8_t *)((uint64_t)phys + hhdm);
        memset(new_data, 0, new_cap);
        if (file->data && node->length > 0) {
            uint32_t copy_len = node->length < file->capacity ? node->length : file->capacity;
            memcpy(new_data, file->data, copy_len);
            ramfs_free_file_data(file);
        }
        klog_debug_puts("[MEMFD_MMAP] backing -> PMM ptr=");
        klog_debug_hex64((uint64_t)new_data);
        klog_debug_puts(" old_cap=");
        klog_debug_uint64(old_cap);
        klog_debug_puts(" old_pmm=");
        klog_debug_uint64(old_is_pmm);
        klog_debug_puts(" new_cap=");
        klog_debug_uint64(new_cap);
        klog_debug_puts(" needed=");
        klog_debug_uint64(needed);
        klog_debug_puts("\n");
        file->data     = new_data;
        file->capacity = new_cap;
        file->data_is_pmm = 1;
    }

    if ((uint32_t)needed > node->length) node->length = (uint32_t)needed;

    struct thread *t = sched_get_current();
    if (!t) return (uint64_t)-1;

    uint64_t *pml4  = vmm_get_active_pml4();
    uint64_t vaddr  = addr ? addr : mm_alloc_mmap_region(length);
    if (vaddr == 0) return (uint64_t)-12;

    uint64_t page_flags = PAGE_FLAG_PRESENT | PAGE_FLAG_USER;
    if (prot & 0x2) page_flags |= PAGE_FLAG_RW;
    if (!(prot & 0x4)) page_flags |= PAGE_FLAG_NX;

    for (uint64_t off = 0; off < length; off += 0x1000) {
        uint64_t data_off = offset + off;
        if (data_off >= file->capacity) break;
        uint64_t kva  = (uint64_t)(file->data + data_off);
        uint64_t phys = kva - hhdm;
        vmm_map_page(pml4, vaddr + off, phys, page_flags);
    }

    klog_debug_puts("[MEMFD_MMAP] mapped "); klog_debug_uint64(length);
    klog_debug_puts(" bytes at ");           klog_debug_uint64(vaddr); klog_debug_puts("\n");
    return vaddr;
}

static uint64_t sys_memfd_create(uint64_t name_ptr, uint64_t flags_arg,
                                  uint64_t a2, uint64_t a3, uint64_t a4,
                                  uint64_t a5) {
    (void)a2; (void)a3; (void)a4; (void)a5;
    struct thread *t = sched_get_current();
    if (!t) return (uint64_t)-1;

    int fd = alloc_fd(t);
    if (fd < 0) return (uint64_t)-24;

    char node_name[128];
    if (name_ptr && is_user_ptr(name_ptr)) {
        strncpy(node_name, "memfd:", 7);
        strncat(node_name, (const char *)name_ptr, sizeof(node_name) - 8);
        node_name[sizeof(node_name) - 1] = '\0';
    } else {
        strcpy(node_name, "memfd:(anonymous)");
    }

    vfs_node_t *node = kmalloc(sizeof(vfs_node_t));
    if (!node) return (uint64_t)-12;
    vfs_node_init(node);
    strncpy(node->name, node_name, 127);
    node->name[127] = '\0';
    node->flags  = FS_FILE;
    node->mask   = 0600;
    node->length = 0;

    ramfs_file_t *rfile = kmalloc(sizeof(ramfs_file_t));
    if (!rfile) { kfree(node); return (uint64_t)-12; }
    rfile->data     = NULL;
    rfile->capacity = 0;
    rfile->data_is_pmm = 1;

    node->device    = rfile;
    node->read      = ramfs_read;
    node->write     = ramfs_write;
    node->truncate  = ramfs_truncate;
    node->fallocate = ramfs_fallocate;
    node->mmap      = memfd_mmap;
    node->close     = memfd_close;

    t->fds[fd]        = node;
    t->fd_offsets[fd] = 0;
    /* Set the descriptor flags explicitly: the slot may still hold the flags of
     * whatever descriptor number was reused. */
    t->fd_flags[fd]   = (flags_arg & MFD_CLOEXEC) ? FD_FLAGS_CLOEXEC_BIT : 0;
    fd_path_set(t, fd, node_name);

    return (uint64_t)fd;
}

// ---------------------------------------------------------------------------
// Registration
// ---------------------------------------------------------------------------

void syscall_register_aio(void) {
    syscall_register(SYS_EVENTFD2,         sys_eventfd2);
    syscall_register(SYS_TIMERFD_CREATE,   sys_timerfd_create);
    syscall_register(SYS_TIMERFD_SETTIME,  sys_timerfd_settime);
    syscall_register(SYS_TIMERFD_GETTIME,  sys_timerfd_gettime);
    syscall_register(SYS_PIPE,             sys_pipe);
    syscall_register(SYS_PIPE2,            sys_pipe2);
    syscall_register(SYS_INOTIFY_INIT,     sys_inotify_init);
    syscall_register(SYS_INOTIFY_INIT1,    sys_inotify_init1);
    syscall_register(SYS_INOTIFY_ADD_WATCH, sys_inotify_add_watch);
    syscall_register(SYS_INOTIFY_RM_WATCH,  sys_inotify_rm_watch);
    syscall_register(SYS_MEMFD_CREATE,     sys_memfd_create);
}
