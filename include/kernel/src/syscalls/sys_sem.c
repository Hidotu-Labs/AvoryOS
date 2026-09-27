// System V Semaphore Syscalls (semget, semop, semctl, semtimedop)
#include "../console/console.h"
#include "../console/klog.h"
#include "../lib/string.h"
#include "../lock/spinlock.h"
#include "../mm/heap.h"
#include "../sched/sched.h"
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

void sem_init(void) {
    memset(sem_table, 0, sizeof(sem_table));
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
            if (sem_table[i].active && sem_table[i].key == (uint32_t)key) {
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

    struct sem_set *set = &sem_table[slot];
    memset(set, 0, sizeof(*set));
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

    struct sembuf *sops = (struct sembuf *)sops_addr;

    spinlock_acquire(&sem_lock);

    struct sem_set *set = NULL;
    for (int i = 0; i < SEM_MAX_SETS; i++) {
        if (sem_table[i].active && sem_table[i].semid == (uint32_t)semid) {
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
            }
            spinlock_release(&sem_lock);
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

        if (nowait || timeout_addr != 0) {
            spinlock_release(&sem_lock);
            return -11; // EAGAIN
        }

        // Yield CPU to allow other threads to post/signal
        spinlock_release(&sem_lock);
        sched_yield();
        spinlock_acquire(&sem_lock);

        // Recheck if set was destroyed while waiting
        if (!set->active) {
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

    spinlock_acquire(&sem_lock);

    struct sem_set *set = NULL;
    for (int i = 0; i < SEM_MAX_SETS; i++) {
        if (sem_table[i].active && sem_table[i].semid == (uint32_t)semid) {
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
        spinlock_release(&sem_lock);
        return 0;

    case IPC_STAT:
    case SEM_STAT:
        if (arg != 0) {
            struct semid_ds_raw *ds = (struct semid_ds_raw *)arg;
            memset(ds, 0, sizeof(*ds));
            ds->sem_perm.__key = set->key;
            ds->sem_perm.uid = set->creator_uid;
            ds->sem_perm.gid = set->creator_gid;
            ds->sem_perm.mode = set->perm_mode;
            ds->sem_nsems = set->nsems;
            ds->sem_otime = set->otime;
            ds->sem_ctime = set->ctime;
            spinlock_release(&sem_lock);
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
        spinlock_release(&sem_lock);
        return 0;

    case GETALL:
        if (arg == 0) {
            spinlock_release(&sem_lock);
            return -14;
        }
        {
            uint16_t *array = (uint16_t *)arg;
            for (uint16_t i = 0; i < set->nsems; i++) {
                array[i] = set->sems[i].val;
            }
            spinlock_release(&sem_lock);
            return 0;
        }

    case SETALL:
        if (arg == 0) {
            spinlock_release(&sem_lock);
            return -14;
        }
        {
            uint16_t *array = (uint16_t *)arg;
            for (uint16_t i = 0; i < set->nsems; i++) {
                set->sems[i].val = array[i];
            }
            spinlock_release(&sem_lock);
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
