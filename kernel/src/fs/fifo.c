// fifo.c — POSIX Named Pipe (FIFO) Implementation for AvoryOS

#include "fifo.h"
#include "ramfs.h"
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

typedef struct fifo_ctx {
    ramfs_file_t ramfs;
    wait_queue_t data_wq; // Readers/writers waiting for data/buffer space
    wait_queue_t open_wq; // Waiting for peer in open()
    spinlock_t   lock;
    uint32_t length;
    uint32_t read_offset;
    uint32_t readers;
    uint32_t writers;
    uint32_t total_readers;
    uint32_t total_writers;
    uint32_t refcount;
    vfs_node_t *read_node; // For epoll notifications
    vfs_node_t *write_node; // For epoll notifications
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
    while (fifo->read_offset >= fifo->length) {
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
        if (fifo->read_offset < fifo->length || fifo->writers == 0) {
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

    vfs_node_t storage = {0};
    storage.flags = FS_PIPE;
    storage.device = &fifo->ramfs;
    storage.length = fifo->length;
    int32_t ret = (int32_t)ramfs_read(&storage, fifo->read_offset, size, buffer);
    vfs_node_t *wnode = NULL;
    if (ret > 0) {
        fifo->read_offset += (uint32_t)ret;
        if (fifo->read_offset >= fifo->length) {
            fifo->read_offset = 0;
            fifo->length = 0;
            if (fifo->writers == 0 || fifo->ramfs.data_is_pmm || fifo->ramfs.capacity > 4096) {
                ramfs_free_file_data(&fifo->ramfs);
            }
        }
        wnode = fifo->writers > 0 ? fifo->write_node : NULL;
        wait_queue_wake_all(&fifo->data_wq);
    }
    spinlock_release(&fifo->lock);
    if (wnode)
        epoll_notify_event(wnode, POLLOUT | POLLWRNORM);
    return (uint32_t)ret;
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
    while ((fifo->length - fifo->read_offset) >= FIFO_MAX_BUFFER) {
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
        if ((fifo->length - fifo->read_offset) < FIFO_MAX_BUFFER || fifo->readers == 0) {
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

    // Compact unread data to offset 0
    if (fifo->read_offset > 0) {
        uint32_t unread = fifo->length - fifo->read_offset;
        if (unread > 0 && fifo->ramfs.data) {
            memmove(fifo->ramfs.data, fifo->ramfs.data + fifo->read_offset, unread);
        }
        fifo->length = unread;
        fifo->read_offset = 0;
        if (unread == 0 && (fifo->ramfs.data_is_pmm || fifo->ramfs.capacity > 4096)) {
            ramfs_free_file_data(&fifo->ramfs);
        }
    }

    vfs_node_t storage = {0};
    storage.flags = FS_PIPE;
    storage.device = &fifo->ramfs;
    storage.length = fifo->length;
    int32_t ret = (int32_t)ramfs_write(&storage, fifo->length, size, buffer);
    if (ret > 0) {
        fifo->length = storage.length;
        vfs_node_t *rnode = fifo->readers > 0 ? fifo->read_node : NULL;
        spinlock_release(&fifo->lock);
        wait_queue_wake_all(&fifo->data_wq);
        if (rnode)
            epoll_notify_event(rnode, POLLIN | POLLRDNORM);
        return (uint32_t)ret;
    }
    spinlock_release(&fifo->lock);
    return ret < 0 ? (uint32_t)ret : (uint32_t)-28; // -ENOSPC (never 0)
}

static int fifo_poll(vfs_node_t *node, int events) {
    fifo_end_t *end = (fifo_end_t *)node->device;
    if (!end || !end->fifo) return 0;
    fifo_ctx_t *fifo = end->fifo;
    int revents = 0;

    spinlock_acquire(&fifo->lock);
    if (end->is_reader) {
        if (fifo->length > fifo->read_offset) revents |= (POLLIN | POLLRDNORM);
        if (fifo->writers == 0) revents |= (POLLIN | POLLRDNORM | POLLHUP);
    }
    if (end->is_writer) {
        if (fifo->readers == 0) revents |= POLLERR;
        // Linux reports POLLOUT only while at least one byte fits; always-ready
        // POLLOUT on a full 64KB FIFO makes level-triggered epoll_wait return
        // immediately forever (wireplumber 100% CPU).
        if (fifo->readers > 0 &&
            (fifo->length - fifo->read_offset) < FIFO_MAX_BUFFER)
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
        int unread = 0;
        if (fifo->length > fifo->read_offset) {
            unread = (int)(fifo->length - fifo->read_offset);
        }
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
        if (fifo->writers == 0 && fifo->length == 0 && fifo->ramfs.data) {
            fifo->read_offset = 0;
            ramfs_free_file_data(&fifo->ramfs);
        } else if (fifo->readers == 0 && fifo->writers == 0) {
            fifo->read_offset = 0;
            fifo->length = 0;
            ramfs_free_file_data(&fifo->ramfs);
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
            ramfs_free_file_data(&fifo->ramfs);
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
        ramfs_free_file_data(&fifo->ramfs);
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
