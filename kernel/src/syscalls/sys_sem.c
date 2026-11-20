// System V Semaphore Syscalls (semget, semop, semctl, semtimedop)
#include "../console/console.h"
#include "../console/klog.h"
#include "../lib/string.h"
#include "../lock/spinlock.h"
#include "../mm/heap.h"
#include "../sched/sched.h"
#include "../sched/namespace.h"
#include "../include/arch/uaccess.h"
#include "../sched/wait.h"
#include "../apic/lapic_timer.h"
#include "syscall.h"
#include <stdbool.h>
#include <stdint.h>

#define SEM_MAX_SETS 64
#define SEM_MAX_SEMS_PER_SET 256

#define IPC_CREAT  01000
#define IPC_EXCL   02000
#define IPC_NOWAIT 04000

#define IPC_RMID 0
#define IPC_SET  1
#define IPC_STAT 2
#define IPC_INFO 3

#define GETPID  11
#define GETVAL  12
#define GETALL  13
#define GETNCNT 14
#define GETZCNT 15
#define SETVAL  16
#define SETALL  17
#define SEM_STAT 18
#define SEM_INFO 19

#define SEM_UNDO 0x1000

struct sembuf {
    uint16_t sem_num; // semaphore index in set
    int16_t  sem_op;  // semaphore operation
    int16_t  sem_flg; // operation flags
};

struct ipc_perm_raw {
    uint32_t __key;
    uint32_t uid;
    uint32_t gid;
    uint32_t cuid;
    uint32_t cgid;
    uint32_t mode;
    uint16_t __seq;
    uint16_t __pad1;
    uint64_t __pad2;
};

struct semid_ds_raw {
    struct ipc_perm_raw sem_perm;
    uint64_t sem_otime;
    uint64_t __pad1;
    uint64_t sem_ctime;
    uint64_t __pad2;
    uint64_t sem_nsems;
    uint64_t __pad3;
    uint64_t __pad4;
};

struct sem_val {
    uint16_t val;
    uint32_t lpid;
};

struct sem_set {
    bool active;
    bool marked_destroy;
    uint32_t namespace_id;
    uint32_t key;
    uint32_t semid;
    uint16_t nsems;
    uint32_t perm_mode;
    uint32_t creator_uid;
    uint32_t creator_gid;
    uint32_t creator_pid;
    uint64_t otime;
    uint64_t ctime;
    struct sem_val sems[SEM_MAX_SEMS_PER_SET];
};

static spinlock_t sem_lock = SPINLOCK_INIT;
static struct sem_set sem_table[SEM_MAX_SETS];
static uint32_t sem_next_id = 1;
/* Blocked semop callers sleep here; every value change wakes them to retry. */
static wait_queue_t sem_wq;

/* SEM_UNDO: per-process adjustments applied when the process exits, so a
 * holder that dies (or simply exits without releasing) does not leave the
 * semaphore taken forever. Steam's single-instance lock depends on this. */
#define SEM_UNDO_MAX 256
struct sem_undo {
    bool used;
    uint32_t tgid;
    uint32_t semid;
    int16_t adj[SEM_MAX_SEMS_PER_SET];
};
static struct sem_undo sem_undo_table[SEM_UNDO_MAX];

/* Called with sem_lock held. */
static struct sem_undo *sem_undo_get(uint32_t tgid, uint32_t semid, bool create) {
    struct sem_undo *free_slot = NULL;
    for (int i = 0; i < SEM_UNDO_MAX; i++) {
        struct sem_undo *u = &sem_undo_table[i];
        if (u->used && u->tgid == tgid && u->semid == semid) return u;
        if (!u->used && !free_slot) free_slot = u;
    }
    if (!create || !free_slot) return NULL;
    memset(free_slot, 0, sizeof(*free_slot));
    free_slot->used = true;
    free_slot->tgid = tgid;
    free_slot->semid = semid;
    return free_slot;
}

/* Called with sem_lock held: SETVAL/SETALL/IPC_RMID invalidate adjustments. */
static void sem_undo_clear(uint32_t semid, int semnum) {
    for (int i = 0; i < SEM_UNDO_MAX; i++) {
        struct sem_undo *u = &sem_undo_table[i];
        if (!u->used || u->semid != semid) continue;
        if (semnum < 0) u->used = false;
        else u->adj[semnum] = 0;
    }
}

void sem_init(void) {
    memset(sem_table, 0, sizeof(sem_table));
    memset(sem_undo_table, 0, sizeof(sem_undo_table));
    wait_queue_init(&sem_wq);
    sem_next_id = 1;
    klog_puts("[SEM] System V Semaphore subsystem initialized.\n");
}

int64_t sys_semget(uint64_t key, uint64_t nsems, uint64_t semflg, uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a3; (void)a4; (void)a5;

    if (nsems > SEM_MAX_SEMS_PER_SET) {
        return -22; // EINVAL
    }

    spinlock_acquire(&sem_lock);

    // If key != IPC_PRIVATE (0), check if set already exists
    if (key != 0) {
        for (int i = 0; i < SEM_MAX_SETS; i++) {
            if (sem_table[i].active &&
                sem_table[i].namespace_id == ipc_namespace_id(sched_get_current()) &&
                sem_table[i].key == (uint32_t)key) {
                if ((semflg & IPC_CREAT) && (semflg & IPC_EXCL)) {
                    spinlock_release(&sem_lock);
                    return -17; // EEXIST
                }
                if (nsems > 0 && sem_table[i].nsems < nsems) {
                    spinlock_release(&sem_lock);
                    return -22; // EINVAL
                }
                uint32_t id = sem_table[i].semid;
                spinlock_release(&sem_lock);
                return (int64_t)id;
            }
        }

        // Not found, must have IPC_CREAT
        if (!(semflg & IPC_CREAT)) {
            spinlock_release(&sem_lock);
            return -2; // ENOENT
        }
    }

    if (nsems == 0) {
        spinlock_release(&sem_lock);
        return -22; // EINVAL
    }

    // Find free slot
    int slot = -1;
    for (int i = 0; i < SEM_MAX_SETS; i++) {
        if (!sem_table[i].active) {
            slot = i;
            break;
        }
    }

    if (slot == -1) {
        spinlock_release(&sem_lock);
        return -28; // ENOSPC
    }

    if (sem_next_id > 0x7FFFFFFFU) { spinlock_release(&sem_lock); return -28; }
    struct sem_set *set = &sem_table[slot];
    memset(set, 0, sizeof(*set));
    set->namespace_id = ipc_namespace_id(sched_get_current());
    set->active = true;
    set->key = (uint32_t)key;
    set->semid = sem_next_id++;
    set->nsems = (uint16_t)nsems;
    set->perm_mode = (uint32_t)(semflg & 0x1FF);

    struct thread *t = sched_get_current();
    if (t) {
        set->creator_uid = t->euid;
        set->creator_gid = t->egid;
        set->creator_pid = t->tgid;
    }

    uint32_t id = set->semid;
    spinlock_release(&sem_lock);
    return (int64_t)id;
}

int64_t sys_semtimedop(uint64_t semid, uint64_t sops_addr, uint64_t nsops, uint64_t timeout_addr, uint64_t a4, uint64_t a5) {
    (void)a4; (void)a5;
    if (nsops == 0 || nsops > SEM_MAX_SEMS_PER_SET || !sops_addr) {
        return -22; // EINVAL
    }

    size_t sops_bytes = (size_t)nsops * sizeof(struct sembuf);
    if (!is_user_range((const void *)sops_addr, sops_bytes)) {
        return -14; // EFAULT
    }

    struct sembuf sops[SEM_MAX_SEMS_PER_SET];
    if (copy_from_user(sops, (const void *)sops_addr, sops_bytes) != 0) {
        return -14; // EFAULT
    }

    /* Relative timeout. i386 ipc(SEMTIMEDOP) passes a 32-bit timespec. */
    uint64_t deadline = 0;
    if (timeout_addr) {
        int64_t sec, nsec;
        struct thread *caller = sched_get_current();
        if (caller && caller->seccomp_compat_dispatch) {
            int32_t ts32[2];
            if (copy_from_user(ts32, (const void *)timeout_addr, sizeof(ts32)) != 0)
                return -14;
            sec = ts32[0]; nsec = ts32[1];
        } else {
            int64_t ts64[2];
            if (copy_from_user(ts64, (const void *)timeout_addr, sizeof(ts64)) != 0)
                return -14;
            sec = ts64[0]; nsec = ts64[1];
        }
        if (sec < 0 || nsec < 0 || nsec >= 1000000000)
            return -22; // EINVAL
        uint64_t ms = (uint64_t)sec * 1000 + ((uint64_t)nsec + 999999) / 1000000;
        deadline = lapic_timer_get_ticks() + ms;
    }

    spinlock_acquire(&sem_lock);

    struct sem_set *set = NULL;
    for (int i = 0; i < SEM_MAX_SETS; i++) {
        if (sem_table[i].active &&
            sem_table[i].namespace_id == ipc_namespace_id(sched_get_current()) &&
            sem_table[i].semid == (uint32_t)semid) {
            set = &sem_table[i];
            break;
        }
    }

    if (!set) {
        spinlock_release(&sem_lock);
        return -22; // EINVAL
    }

    // Verify all sem_nums in bounds
    for (uint64_t i = 0; i < nsops; i++) {
        if (sops[i].sem_num >= set->nsems) {
            spinlock_release(&sem_lock);
            return -33; // EFBIG
        }
    }

    struct thread *t = sched_get_current();
    uint32_t pid = t ? t->tgid : 0;

    // Retry loop for blocking operations
    while (true) {
        bool can_perform = true;

        for (uint64_t i = 0; i < nsops; i++) {
            int num = sops[i].sem_num;
            int op = sops[i].sem_op;

            if (op < 0) {
                if (set->sems[num].val < (uint16_t)(-op)) {
                    can_perform = false;
                    break;
                }
            } else if (op == 0) {
                if (set->sems[num].val != 0) {
                    can_perform = false;
                    break;
                }
            }
        }

        if (can_perform) {
            // Apply all operations atomically
            for (uint64_t i = 0; i < nsops; i++) {
                int num = sops[i].sem_num;
                int op = sops[i].sem_op;

                if (op < 0) {
                    set->sems[num].val -= (uint16_t)(-op);
                } else if (op > 0) {
                    set->sems[num].val += (uint16_t)op;
                }
                set->sems[num].lpid = pid;
                if (op != 0 && (sops[i].sem_flg & SEM_UNDO)) {
                    struct sem_undo *u = sem_undo_get(pid, set->semid, true);
                    if (u) u->adj[num] = (int16_t)(u->adj[num] - op);
                }
            }
            set->otime = lapic_timer_get_ticks() / 1000;
            spinlock_release(&sem_lock);
            wait_queue_wake_all(&sem_wq);
            return 0;
        }

        // Check if nonblocking requested
        bool nowait = false;
        for (uint64_t i = 0; i < nsops; i++) {
            if (sops[i].sem_flg & IPC_NOWAIT) {
                nowait = true;
                break;
            }
        }

        if (nowait) {
            spinlock_release(&sem_lock);
            return -11; // EAGAIN
        }
        if (deadline && lapic_timer_get_ticks() >= deadline) {
            spinlock_release(&sem_lock);
            return -11; // EAGAIN: semtimedop timed out
        }
        if (t && thread_has_pending_signal(t)) {
            spinlock_release(&sem_lock);
            return -4; // EINTR
        }

        /* Sleep until a value changes (or the deadline). Queue while still
         * holding sem_lock so a concurrent post cannot slip past unseen. */
        if (!t) {
            spinlock_release(&sem_lock);
            return -11;
        }
        wait_queue_entry_t entry = {.thread = t};
        t->state = THREAD_BLOCKED;
        t->wakeup_ticks = deadline;
        wait_queue_add(&sem_wq, &entry);
        spinlock_release(&sem_lock);
        sched_yield();
        t->state = THREAD_RUNNING;
        t->wakeup_ticks = 0;
        wait_queue_remove(&sem_wq, &entry);
        spinlock_acquire(&sem_lock);

        // Recheck if set was destroyed while waiting
        if (!set->active || set->semid != (uint32_t)semid ||
            set->namespace_id != ipc_namespace_id(sched_get_current())) {
            spinlock_release(&sem_lock);
            return -43; // EIDRM
        }
    }
}

int64_t sys_semop(uint64_t semid, uint64_t sops_addr, uint64_t nsops, uint64_t a3, uint64_t a4, uint64_t a5) {
    return sys_semtimedop(semid, sops_addr, nsops, 0, a3, a4);
}

int64_t sys_semctl(uint64_t semid, uint64_t semnum, uint64_t cmd, uint64_t arg, uint64_t a4, uint64_t a5) {
    (void)a4; (void)a5;
    /* IPC_64: glibc ORs it into every command (always on i386). Without
     * stripping it SETVAL hit the EINVAL default, so a lock semaphore that
     * Steam creates and initialises to 1 stayed at 0 and its first semop
     * blocked forever. sys_shmctl strips it the same way. */
    cmd &= ~0x100ULL;

    spinlock_acquire(&sem_lock);

    struct sem_set *set = NULL;
    for (int i = 0; i < SEM_MAX_SETS; i++) {
        if (sem_table[i].active &&
            sem_table[i].namespace_id == ipc_namespace_id(sched_get_current()) &&
            sem_table[i].semid == (uint32_t)semid) {
            set = &sem_table[i];
            break;
        }
    }

    if (!set) {
        spinlock_release(&sem_lock);
        return -22; // EINVAL
    }

    switch (cmd) {
    case IPC_RMID:
        set->active = false;
        sem_undo_clear(set->semid, -1);
        spinlock_release(&sem_lock);
        wait_queue_wake_all(&sem_wq); // waiters return EIDRM
        return 0;

    case IPC_STAT:
    case SEM_STAT:
        if (arg != 0) {
            struct semid_ds_raw ds;
            memset(&ds, 0, sizeof(ds));
            ds.sem_perm.__key = set->key;
            ds.sem_perm.uid = set->creator_uid;
            ds.sem_perm.gid = set->creator_gid;
            ds.sem_perm.mode = set->perm_mode;
            ds.sem_nsems = set->nsems;
            ds.sem_otime = set->otime;
            ds.sem_ctime = set->ctime;
            spinlock_release(&sem_lock);
            if (!is_user_range((const void *)arg, sizeof(ds)) ||
                copy_to_user((void *)arg, &ds, sizeof(ds)) != 0) {
                return -14; // EFAULT
            }
            return 0;
        }
        spinlock_release(&sem_lock);
        return -14; // EFAULT

    case GETVAL:
        if (semnum >= set->nsems) {
            spinlock_release(&sem_lock);
            return -22; // EINVAL
        }
        {
            int val = (int)set->sems[semnum].val;
            spinlock_release(&sem_lock);
            return val;
        }

    case SETVAL:
        if (semnum >= set->nsems) {
            spinlock_release(&sem_lock);
            return -22; // EINVAL
        }
        set->sems[semnum].val = (uint16_t)(arg & 0xFFFF);
        sem_undo_clear(set->semid, (int)semnum);
        spinlock_release(&sem_lock);
        wait_queue_wake_all(&sem_wq);
        return 0;

    case GETALL:
        if (arg == 0) {
            spinlock_release(&sem_lock);
            return -14;
        }
        {
            uint16_t vals[SEM_MAX_SEMS_PER_SET];
            uint16_t nsems = set->nsems;
            for (uint16_t i = 0; i < nsems; i++) {
                vals[i] = set->sems[i].val;
            }
            spinlock_release(&sem_lock);
            size_t bytes = (size_t)nsems * sizeof(uint16_t);
            if (!is_user_range((const void *)arg, bytes) ||
                copy_to_user((void *)arg, vals, bytes) != 0) {
                return -14;
            }
            return 0;
        }

    case SETALL:
        if (arg == 0) {
            spinlock_release(&sem_lock);
            return -14;
        }
        {
            uint16_t vals[SEM_MAX_SEMS_PER_SET];
            uint16_t nsems = set->nsems;
            size_t bytes = (size_t)nsems * sizeof(uint16_t);
            spinlock_release(&sem_lock);
            if (!is_user_range((const void *)arg, bytes) ||
                copy_from_user(vals, (const void *)arg, bytes) != 0) {
                return -14;
            }
            spinlock_acquire(&sem_lock);
            if (!set->active || set->nsems != nsems) {
                spinlock_release(&sem_lock);
                return -22;
            }
            for (uint16_t i = 0; i < nsems; i++) {
                set->sems[i].val = vals[i];
            }
            sem_undo_clear(set->semid, -1);
            spinlock_release(&sem_lock);
            wait_queue_wake_all(&sem_wq);
            return 0;
        }

    case GETPID:
        if (semnum >= set->nsems) {
            spinlock_release(&sem_lock);
            return -22;
        }
        {
            int pid = (int)set->sems[semnum].lpid;
            spinlock_release(&sem_lock);
            return pid;
        }

    case GETNCNT:
    case GETZCNT:
        spinlock_release(&sem_lock);
        return 0;

    default:
        spinlock_release(&sem_lock);
        return -22; // EINVAL
    }
}

void syscall_register_sem(void) {
    syscall_register(64, (syscall_handler_t)sys_semget);
    syscall_register(65, (syscall_handler_t)sys_semop);
    syscall_register(66, (syscall_handler_t)sys_semctl);
    syscall_register(220, (syscall_handler_t)sys_semtimedop);
    klog_puts("[SEM] Syscalls registered (semget=64, semop=65, semctl=66, semtimedop=220).\n");
}

/* Process exit: apply this process's SEM_UNDO adjustments (clamped to the
 * valid range, like Linux) and drop them. */
void sem_exit(uint32_t tgid) {
    bool changed = false;
    spinlock_acquire(&sem_lock);
    for (int i = 0; i < SEM_UNDO_MAX; i++) {
        struct sem_undo *u = &sem_undo_table[i];
        if (!u->used || u->tgid != tgid) continue;
        for (int s = 0; s < SEM_MAX_SETS; s++) {
            struct sem_set *set = &sem_table[s];
            if (!set->active || set->semid != u->semid) continue;
            for (uint16_t n = 0; n < set->nsems; n++) {
                if (!u->adj[n]) continue;
                int32_t v = (int32_t)set->sems[n].val + u->adj[n];
                if (v < 0) v = 0;
                if (v > 32767) v = 32767;
                set->sems[n].val = (uint16_t)v;
                set->sems[n].lpid = tgid;
                changed = true;
            }
        }
        u->used = false;
    }
    spinlock_release(&sem_lock);
    if (changed) wait_queue_wake_all(&sem_wq);
}

void sem_namespace_destroy(uint32_t id) {
    spinlock_acquire(&sem_lock);
    for (int i = 0; i < SEM_MAX_SETS; i++)
        if (sem_table[i].active && sem_table[i].namespace_id == id)
            sem_table[i].active = false;
    spinlock_release(&sem_lock);
}
