// fifo.c — POSIX Named Pipe (FIFO) Implementation for AvoryOS

#include "fifo.h"
#include "vfs.h"
#include "../mm/heap.h"
#include "../mm/vmm.h"
#include "../sched/sched.h"
#include "../sched/wait.h"
#include "../socket/epoll.h"
#include "../syscalls/sys_io_shared.h"
#include "../console/klog.h"
#include <stdbool.h>
#include <stdint.h>
#include "../lib/string.h"

#define FIFO_MAX_BUFFER 65536
#define FIFO_BUF_MASK   (FIFO_MAX_BUFFER - 1)

typedef struct fifo_ctx {
    uint8_t     *buffer;
    wait_queue_t data_wq; // Readers/writers waiting for data/buffer space
    wait_queue_t open_wq; // Waiting for peer in open()
    spinlock_t   lock;
    uint32_t     head;
    uint32_t     tail;
    uint32_t     count;
    uint32_t     readers;
    uint32_t     writers;
    uint32_t     total_readers;
    uint32_t     total_writers;
    uint32_t     refcount;
    vfs_node_t  *read_node; // For epoll notifications
    vfs_node_t  *write_node; // For epoll notifications
} fifo_ctx_t;

typedef struct {
    fifo_ctx_t *fifo;
    bool is_reader;
    bool is_writer;
} fifo_end_t;

static spinlock_t fifo_init_lock = SPINLOCK_INIT;

static uint32_t fifo_read(vfs_node_t *node, uint32_t offset, uint32_t size, uint8_t *buffer) {
    (void)offset;
    if (size == 0) return 0;
    fifo_end_t *end = (fifo_end_t *)node->device;
    if (!end || !end->fifo) return (uint32_t)-9; // EBADF
    fifo_ctx_t *fifo = end->fifo;

    spinlock_acquire(&fifo->lock);
    while (fifo->count == 0) {
        struct thread *t = sched_get_current();
        int fd = -1;
        if (t) {
            for (uint32_t i = 0; i < t->fd_capacity; i++) {
                if (t->fds[i] == node) { fd = (int)i; break; }
            }
        }
        // If no writers remain, an empty FIFO yields EOF (0)
        if (fifo->writers == 0) {
            spinlock_release(&fifo->lock);
            return 0;
        }
        if (t && fd != -1 && (t->fd_flags[fd] & O_NONBLOCK)) {
            spinlock_release(&fifo->lock);
            return (uint32_t)-11; // -EAGAIN
        }
        if (t && (t->pending_signals & ~t->signal_mask)) {
            spinlock_release(&fifo->lock);
            return (uint32_t)-4; // -EINTR
        }

        wait_queue_entry_t entry = {.thread = t, .next = NULL, .wq = NULL, .thread_next = NULL, .thread_prev = NULL};
        t->state = THREAD_BLOCKED;
        wait_queue_add(&fifo->data_wq, &entry);
        if (fifo->count > 0 || fifo->writers == 0) {
            t->state = THREAD_RUNNING;
            spinlock_release(&fifo->lock);
            wait_queue_remove(&fifo->data_wq, &entry);
            t->state = THREAD_RUNNING;
            spinlock_acquire(&fifo->lock);
            continue;
        }
        spinlock_release(&fifo->lock);
        sched_yield();
        t->state = THREAD_RUNNING;
        wait_queue_remove(&fifo->data_wq, &entry);
        spinlock_acquire(&fifo->lock);
    }

    /* O(1) contiguous block read from circular ring buffer */
    uint32_t to_read = size < fifo->count ? size : fifo->count;
    uint32_t first_chunk = FIFO_MAX_BUFFER - fifo->tail;
    if (first_chunk > to_read)
        first_chunk = to_read;

    memcpy(buffer, fifo->buffer + fifo->tail, first_chunk);
    uint32_t second_chunk = to_read - first_chunk;
    if (second_chunk > 0) {
        memcpy(buffer + first_chunk, fifo->buffer, second_chunk);
    }

    fifo->tail = (fifo->tail + to_read) & FIFO_BUF_MASK;
    fifo->count -= to_read;

    if (fifo->count == 0) {
        fifo->head = 0;
        fifo->tail = 0;
        if (fifo->writers == 0 && fifo->buffer) {
            kfree(fifo->buffer);
            fifo->buffer = NULL;
        }
    }

    vfs_node_t *wnode = fifo->writers > 0 ? fifo->write_node : NULL;
    if (__builtin_expect(fifo->data_wq.head != NULL, 0))
        wait_queue_wake_all(&fifo->data_wq);
    spinlock_release(&fifo->lock);

    if (wnode)
        epoll_notify_event(wnode, POLLOUT | POLLWRNORM);
    return to_read;
}

static uint32_t fifo_write(vfs_node_t *node, uint32_t offset, uint32_t size, uint8_t *buffer) {
    (void)offset;
    if (size == 0) return 0;
    fifo_end_t *end = (fifo_end_t *)node->device;
    if (!end || !end->fifo) return (uint32_t)-9; // EBADF
    fifo_ctx_t *fifo = end->fifo;

    spinlock_acquire(&fifo->lock);
    if (fifo->readers == 0) {
        spinlock_release(&fifo->lock);
        struct thread *t = sched_get_current();
        if (t) {
            t->pending_signals |= (1ULL << (SIGPIPE - 1));
        }
        return (uint32_t)-32; // -EPIPE
    }

    // Handle buffer capacity limits
    while (fifo->count >= FIFO_MAX_BUFFER) {
        struct thread *t = sched_get_current();
        int fd = -1;
        if (t) {
            for (uint32_t i = 0; i < t->fd_capacity; i++) {
                if (t->fds[i] == node) { fd = (int)i; break; }
            }
        }
        if (t && fd != -1 && (t->fd_flags[fd] & O_NONBLOCK)) {
            spinlock_release(&fifo->lock);
            return (uint32_t)-11; // -EAGAIN
        }
        if (fifo->readers == 0) {
            spinlock_release(&fifo->lock);
            if (t) t->pending_signals |= (1ULL << (SIGPIPE - 1));
            return (uint32_t)-32;
        }
        if (t && (t->pending_signals & ~t->signal_mask)) {
            spinlock_release(&fifo->lock);
            return (uint32_t)-4; // -EINTR
        }

        wait_queue_entry_t entry = {.thread = t, .next = NULL, .wq = NULL, .thread_next = NULL, .thread_prev = NULL};
        t->state = THREAD_BLOCKED;
        wait_queue_add(&fifo->data_wq, &entry);
        if (fifo->count < FIFO_MAX_BUFFER || fifo->readers == 0) {
            t->state = THREAD_RUNNING;
            spinlock_release(&fifo->lock);
            wait_queue_remove(&fifo->data_wq, &entry);
            t->state = THREAD_RUNNING;
            spinlock_acquire(&fifo->lock);
            continue;
        }
        spinlock_release(&fifo->lock);
        sched_yield();
        t->state = THREAD_RUNNING;
        wait_queue_remove(&fifo->data_wq, &entry);
        spinlock_acquire(&fifo->lock);
    }

    if (!fifo->buffer) {
        fifo->buffer = kmalloc(FIFO_MAX_BUFFER);
        if (!fifo->buffer) {
            spinlock_release(&fifo->lock);
            return (uint32_t)-12; // ENOMEM
        }
        fifo->head = 0;
        fifo->tail = 0;
        fifo->count = 0;
    }

    /* O(1) contiguous block write into circular ring buffer */
    uint32_t avail = FIFO_MAX_BUFFER - fifo->count;
    uint32_t to_write = size < avail ? size : avail;

    uint32_t first_chunk = FIFO_MAX_BUFFER - fifo->head;
    if (first_chunk > to_write)
        first_chunk = to_write;

    memcpy(fifo->buffer + fifo->head, buffer, first_chunk);
    uint32_t second_chunk = to_write - first_chunk;
    if (second_chunk > 0) {
        memcpy(fifo->buffer, buffer + first_chunk, second_chunk);
    }

    fifo->head = (fifo->head + to_write) & FIFO_BUF_MASK;
    fifo->count += to_write;

    vfs_node_t *rnode = fifo->readers > 0 ? fifo->read_node : NULL;
    if (__builtin_expect(fifo->data_wq.head != NULL, 0))
        wait_queue_wake_all(&fifo->data_wq);
    spinlock_release(&fifo->lock);

    if (rnode)
        epoll_notify_event(rnode, POLLIN | POLLRDNORM);
    return to_write;
}

static int fifo_poll(vfs_node_t *node, int events) {
    fifo_end_t *end = (fifo_end_t *)node->device;
    if (!end || !end->fifo) return 0;
    fifo_ctx_t *fifo = end->fifo;
    int revents = 0;

    spinlock_acquire(&fifo->lock);
    if (end->is_reader) {
        if (fifo->count > 0) revents |= (POLLIN | POLLRDNORM);
        if (fifo->writers == 0) revents |= (POLLIN | POLLRDNORM | POLLHUP);
    }
    if (end->is_writer) {
        if (fifo->readers == 0) revents |= POLLERR;
        // Linux reports POLLOUT only while at least one byte fits; always-ready
        // POLLOUT on a full 64KB FIFO makes level-triggered epoll_wait return
        // immediately forever (wireplumber 100% CPU).
        if (fifo->readers > 0 && fifo->count < FIFO_MAX_BUFFER)
            revents |= (POLLOUT | POLLWRNORM);
    }
    spinlock_release(&fifo->lock);

    return revents & (events | POLLERR | POLLHUP | POLLNVAL);
}

static int fifo_ioctl(vfs_node_t *node, uint32_t request, uint64_t arg) {
    fifo_end_t *end = (fifo_end_t *)node->device;
    if (!end || !end->fifo) return -9;
    fifo_ctx_t *fifo = end->fifo;

    switch (request) {
    case 0x541B: { // FIONREAD / TIOCINQ
        int *out = (int *)arg;
        if (!out || !vmm_is_user_addr_range_valid(arg, sizeof(int))) {
            return -14; // EFAULT
        }
        spinlock_acquire(&fifo->lock);
        int unread = (int)fifo->count;
        spinlock_release(&fifo->lock);
        *out = unread;
        return 0;
    }
    case 0x5421: { // FIONBIO
        int *val = (int *)arg;
        if (!val || !vmm_is_user_addr_range_valid(arg, sizeof(int))) {
            return -14; // EFAULT
        }
        struct thread *t = sched_get_current();
        if (t) {
            for (uint32_t i = 0; i < t->fd_capacity; i++) {
                if (t->fds[i] == node) {
                    if (*val)
                        t->fd_flags[i] |= O_NONBLOCK;
                    else
                        t->fd_flags[i] &= ~O_NONBLOCK;
                }
            }
        }
        return 0;
    }
    default:
        return -25; // ENOTTY
    }
}

static void fifo_close(vfs_node_t *node) {
    if (!node || !node->device) return;
    fifo_end_t *end = (fifo_end_t *)node->device;
    fifo_ctx_t *fifo = end->fifo;
    vfs_node_t *notify_read = NULL;
    bool free_ctx = false;

    if (fifo) {
        spinlock_acquire(&fifo->lock);
        if (end->is_reader) {
            if (fifo->readers > 0) fifo->readers--;
            if (fifo->read_node == node)
                fifo->read_node = NULL;
        }
        if (end->is_writer) {
            if (fifo->writers > 0) fifo->writers--;
            if (fifo->write_node == node)
                fifo->write_node = NULL;
            if (fifo->readers > 0 && fifo->read_node)
                notify_read = fifo->read_node;
        }
        wait_queue_wake_all(&fifo->data_wq);
        wait_queue_wake_all(&fifo->open_wq);
        if (fifo->writers == 0 && fifo->count == 0 && fifo->buffer) {
            kfree(fifo->buffer);
            fifo->buffer = NULL;
            fifo->head = 0;
            fifo->tail = 0;
        } else if (fifo->readers == 0 && fifo->writers == 0) {
            if (fifo->buffer) {
                kfree(fifo->buffer);
                fifo->buffer = NULL;
            }
            fifo->head = 0;
            fifo->tail = 0;
            fifo->count = 0;
        }
        spinlock_release(&fifo->lock);

        if (notify_read)
            epoll_notify_event(notify_read, POLLHUP);

        spinlock_acquire(&fifo_init_lock);
        if (--fifo->refcount == 0) {
            free_ctx = true;
        }
        spinlock_release(&fifo_init_lock);

        if (free_ctx) {
            if (fifo->buffer)
                kfree(fifo->buffer);
            kfree(fifo);
        }
    }

    kfree(end);
    node->device = NULL;
}

void fifo_destroy(vfs_node_t *node) {
    if (!node || !node->device) return;
    spinlock_acquire(&fifo_init_lock);
    fifo_ctx_t *fifo = (fifo_ctx_t *)node->device;
    node->device = NULL;
    bool free_ctx = false;
    if (fifo) {
        if (--fifo->refcount == 0) {
            free_ctx = true;
        }
    }
    spinlock_release(&fifo_init_lock);

    if (free_ctx) {
        if (fifo->buffer)
            kfree(fifo->buffer);
        kfree(fifo);
    }
}

int fifo_open(vfs_node_t *node, uint64_t flags, vfs_node_t **out_node) {
    if (!node || !out_node) return -14;
    *out_node = NULL;

    spinlock_acquire(&fifo_init_lock);
    fifo_ctx_t *fifo = (fifo_ctx_t *)node->device;
    if (!fifo) {
        fifo = kmalloc(sizeof(fifo_ctx_t));
        if (!fifo) {
            spinlock_release(&fifo_init_lock);
            return -12; // ENOMEM
        }
        memset(fifo, 0, sizeof(*fifo));
        spinlock_init(&fifo->lock);
        wait_queue_init(&fifo->data_wq);
        wait_queue_init(&fifo->open_wq);
        fifo->refcount = 1; // 1 for node->device
        node->device = fifo;
        node->destroy = fifo_destroy;
    }
    fifo->refcount++; // 1 for this open instance
    spinlock_release(&fifo_init_lock);

    uint64_t acc = flags & O_ACCMODE;
    bool is_reader = (acc == O_RDONLY || acc == O_RDWR);
    bool is_writer = (acc == O_WRONLY || acc == O_RDWR);
    bool nonblock  = (flags & O_NONBLOCK) != 0;

    // POSIX: O_WRONLY with O_NONBLOCK fails with ENXIO if no reader is open
    if (acc == O_WRONLY && nonblock && fifo->readers == 0) {
        spinlock_acquire(&fifo_init_lock);
        fifo->refcount--;
        spinlock_release(&fifo_init_lock);
        return -6; // ENXIO
    }

    spinlock_acquire(&fifo->lock);
    if (is_reader && !is_writer) {
        fifo->readers++;
        fifo->total_readers++;
        uint32_t start_writers = fifo->total_writers;
        wait_queue_wake_all(&fifo->open_wq);
        if (!nonblock) {
            while (fifo->writers == 0 && fifo->total_writers == start_writers) {
                struct thread *t = sched_get_current();
                if (t && (t->pending_signals & ~t->signal_mask)) {
                    fifo->readers--;
                    spinlock_release(&fifo->lock);
                    spinlock_acquire(&fifo_init_lock);
                    fifo->refcount--;
                    spinlock_release(&fifo_init_lock);
                    return -4; // EINTR
                }
                wait_queue_entry_t entry = {.thread = t, .next = NULL, .wq = NULL, .thread_next = NULL, .thread_prev = NULL};
                t->state = THREAD_BLOCKED;
                wait_queue_add(&fifo->open_wq, &entry);
                if (fifo->writers > 0 || fifo->total_writers > start_writers) {
                    t->state = THREAD_RUNNING;
                    spinlock_release(&fifo->lock);
                    wait_queue_remove(&fifo->open_wq, &entry);
                    t->state = THREAD_RUNNING;
                    spinlock_acquire(&fifo->lock);
                    break;
                }
                spinlock_release(&fifo->lock);
                sched_yield();
                t->state = THREAD_RUNNING;
                wait_queue_remove(&fifo->open_wq, &entry);
                spinlock_acquire(&fifo->lock);
            }
        }
    } else if (is_writer && !is_reader) {
        fifo->writers++;
        fifo->total_writers++;
        uint32_t start_readers = fifo->total_readers;
        wait_queue_wake_all(&fifo->open_wq);
        if (!nonblock) {
            while (fifo->readers == 0 && fifo->total_readers == start_readers) {
                struct thread *t = sched_get_current();
                if (t && (t->pending_signals & ~t->signal_mask)) {
                    fifo->writers--;
                    spinlock_release(&fifo->lock);
                    spinlock_acquire(&fifo_init_lock);
                    fifo->refcount--;
                    spinlock_release(&fifo_init_lock);
                    return -4; // EINTR
                }
                wait_queue_entry_t entry = {.thread = t, .next = NULL, .wq = NULL, .thread_next = NULL, .thread_prev = NULL};
                t->state = THREAD_BLOCKED;
                wait_queue_add(&fifo->open_wq, &entry);
                if (fifo->readers > 0 || fifo->total_readers > start_readers) {
                    t->state = THREAD_RUNNING;
                    spinlock_release(&fifo->lock);
                    wait_queue_remove(&fifo->open_wq, &entry);
                    t->state = THREAD_RUNNING;
                    spinlock_acquire(&fifo->lock);
                    break;
                }
                spinlock_release(&fifo->lock);
                sched_yield();
                t->state = THREAD_RUNNING;
                wait_queue_remove(&fifo->open_wq, &entry);
                spinlock_acquire(&fifo->lock);
            }
        }
    } else {
        fifo->readers++;
        fifo->writers++;
        fifo->total_readers++;
        fifo->total_writers++;
        wait_queue_wake_all(&fifo->open_wq);
    }

    fifo_end_t *end = kmalloc(sizeof(fifo_end_t));
    if (!end) {
        if (is_reader) fifo->readers--;
        if (is_writer) fifo->writers--;
        spinlock_release(&fifo->lock);
        spinlock_acquire(&fifo_init_lock);
        fifo->refcount--;
        spinlock_release(&fifo_init_lock);
        return -12; // ENOMEM
    }
    end->fifo = fifo;
    end->is_reader = is_reader;
    end->is_writer = is_writer;

    vfs_node_t *inst = kmalloc(sizeof(vfs_node_t));
    if (!inst) {
        kfree(end);
        if (is_reader) fifo->readers--;
        if (is_writer) fifo->writers--;
        spinlock_release(&fifo->lock);
        spinlock_acquire(&fifo_init_lock);
        fifo->refcount--;
        spinlock_release(&fifo_init_lock);
        return -12;
    }
    vfs_node_init(inst);
    strncpy(inst->name, node->name, sizeof(inst->name) - 1);
    inst->name[sizeof(inst->name) - 1] = '\0';
    inst->flags = FS_PIPE;
    inst->device = end;
    inst->read = is_reader ? fifo_read : NULL;
    inst->write = is_writer ? fifo_write : NULL;
    inst->poll = fifo_poll;
    inst->ioctl = fifo_ioctl;
    inst->close = fifo_close;
    inst->wait_queue = &fifo->data_wq;
    if (is_reader && !fifo->read_node) fifo->read_node = inst;
    if (is_writer && !fifo->write_node) fifo->write_node = inst;

    spinlock_release(&fifo->lock);
    *out_node = inst;
    return 0;
}
