#include "tlb_shootdown.h"
#include "hal/hal.h"
#include "../apic/lapic.h"
#include "../console/klog.h"
#include "../cpu/isr.h"
#include "../sched/sched.h"
#include "../smp/cpu.h"
#include "pcid.h"
#include "pmm.h"
#include "vmm.h"
#include "../lock/spinlock.h"
#include "../lock/lockdiag.h"
#include "../cpu/features.h"
#include "../cpu/kpf_dump.h"
#include "../drivers/serial.h"
#include "../lib/string.h"
#include "../lib/tsc.h"
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* -------------------------------------------------------------------------
 * Per-CPU TLB shootdown state
 * ------------------------------------------------------------------------- */

static void local_flush(uint64_t addr, uint16_t pcid);
static uint16_t pcid_for_pml4(uint64_t pml4);

/*
 * Global lock for shootdown serialization.
 *
 * This used to be a rawspinlock, on the theory that a holder waiting for
 * acknowledgements must stay interruptible.  That is backwards for the
 * *holder*: since C6 made syscall dispatch interruptible (syscall_entry.asm
 * runs sti), a timer tick can preempt a thread inside the ack wait, leave
 * shootdown_lock held behind a thread that is no longer running, and the
 * next CoW fault on that same CPU then spins here with interrupts masked
 * from the exception gate - the holder can never be rescheduled on the only
 * core that can run it.  Exactly that produced
 * "cpu 0 stopped taking timer ticks ... WAITING-ON lock=shootdown_lock"
 * with every other core idle.
 *
 * A spinlock_t masks interrupts from before the lock is won until release,
 * so the holder cannot be preempted.  Waiters that arrive with interrupts
 * enabled still open them while spinning (see spinlock_acquire), so a CPU
 * that becomes a target of the current holder's IPI can still answer it
 * while it waits for the lock.  The holder itself needs no interrupts - it
 * is the initiator, and cpu_needs_shootdown() never targets self.
 */
static spinlock_t shootdown_lock = SPINLOCK_INIT;

/* Per-CPU pending virtual address (written by initiator, read by target). */
#define MAX_CPUS 64
static volatile uint64_t cpu_shootdown_addr[MAX_CPUS];
static volatile uint8_t  cpu_shootdown_ack[MAX_CPUS];
/* PCID the stale translations are tagged with, or PCID_KERNEL when the
 * initiator could not tell us (it was modifying an address space other than
 * the one loaded on its own CPU). */
static volatile uint16_t cpu_shootdown_pcid[MAX_CPUS];
/* Timing and execution-context snapshots for the current serialized request.
 * The target records entry/ack timestamps on its own CPU; the initiator records
 * the send timestamp immediately before raising the IPI.  These are diagnostic
 * only and are published before the release-store to cpu_shootdown_ack. */
static volatile uint64_t cpu_shootdown_sent_tsc[MAX_CPUS];
static volatile uint64_t cpu_shootdown_entry_tsc[MAX_CPUS];
static volatile uint64_t cpu_shootdown_ack_tsc[MAX_CPUS];
static volatile uint64_t cpu_shootdown_seen_cr3[MAX_CPUS];
static volatile uint64_t cpu_shootdown_seen_tid[MAX_CPUS];
static volatile uint64_t cpu_shootdown_seen_tgid[MAX_CPUS];
static volatile uint64_t cpu_shootdown_interrupted_rip[MAX_CPUS];
static volatile uint64_t cpu_shootdown_interrupted_cs[MAX_CPUS];
static volatile uint64_t cpu_shootdown_interrupted_rflags[MAX_CPUS];
static volatile uint64_t cpu_shootdown_interrupted_rsp[MAX_CPUS];
static volatile uint64_t cpu_shootdown_kernel_stack[MAX_CPUS][4];
static char cpu_shootdown_seen_comm[MAX_CPUS][16];
static volatile uint32_t slow_ack_log_count;

/* Cost counters for /proc/tlb_stats.  Relaxed atomics: these are allowed to be
 * a few increments behind, they only need to be close enough to compare two
 * runs of the same workload. */
static tlb_shootdown_stats_t tlb_stats;

static void tlb_stat_add(uint64_t *field, uint64_t n) {
    __atomic_add_fetch(field, n, __ATOMIC_RELAXED);
}

static void tlb_stat_max(uint64_t *field, uint64_t v) {
    uint64_t cur = __atomic_load_n(field, __ATOMIC_RELAXED);
    while (v > cur &&
           !__atomic_compare_exchange_n(field, &cur, v, false,
                                        __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {
        /* cur reloaded by the CAS */
    }
}

void tlb_shootdown_get_stats(tlb_shootdown_stats_t *out) {
    if (!out)
        return;
    out->shootdowns = __atomic_load_n(&tlb_stats.shootdowns, __ATOMIC_RELAXED);
    out->ipis_sent = __atomic_load_n(&tlb_stats.ipis_sent, __ATOMIC_RELAXED);
    out->broadcast_all =
        __atomic_load_n(&tlb_stats.broadcast_all, __ATOMIC_RELAXED);
    out->handler_full_flushes =
        __atomic_load_n(&tlb_stats.handler_full_flushes, __ATOMIC_RELAXED);
    out->ack_wait_ns =
        __atomic_load_n(&tlb_stats.ack_wait_ns, __ATOMIC_RELAXED);
    out->max_ack_wait_ns =
        __atomic_load_n(&tlb_stats.max_ack_wait_ns, __ATOMIC_RELAXED);
}

void tlb_shootdown_reset_stats(void) {
    tlb_stats = (tlb_shootdown_stats_t){0};
}

static bool cpu_needs_shootdown(struct cpu_info *cpu, struct cpu_info *self,
                                uint64_t addr, uint64_t pcid,
                                uint64_t pml4_base) {
    if (!cpu || cpu == self)
        return false;
    if (cpu->status != CPU_STATUS_ONLINE && cpu->status != CPU_STATUS_BSP)
        return false;

    /* A full invalidation, and every kernel-space mapping, can be cached by
     * any CPU whatever address space it has loaded - kernel page tables are
     * shared by all of them. */
    if (addr == TLB_SHOOTDOWN_ALL ||
        (addr > USER_SPACE_LIMIT && addr != TLB_SHOOTDOWN_CONTEXT))
        return true;

    /* With a known PCID, only a CPU currently running that address space
     * needs an IPI.  An inactive CPU may retain tagged translations, so clear
     * its scheduler bookkeeping while holding queue_lock.  The scheduler
     * checks that bit and loads CR3 with flushing semantics before it can run
     * the address space again.  queue_lock closes the race with the CR3 load:
     * either we observe it active and send an IPI, or its later switch sees
     * the cleared bit and flushes locally.  If the lock is busy, target it
     * conservatively; this keeps shootdown latency independent of runqueue
     * lock contention and preserves correctness.
     *
     * Unknown PCIDs still require a broadcast, because we cannot safely
     * identify which tagged translations belong to this page-table root. */
    if (cpu_has_pcid()) {
        if (pcid == PCID_KERNEL)
            return true;

        if (!spinlock_try_acquire(&cpu->queue_lock))
            return true;

        uint64_t loaded = __atomic_load_n(&cpu->active_cr3, __ATOMIC_ACQUIRE);
        bool active = loaded != 0 &&
                      (loaded & CR3_ADDR_MASK) == (pml4_base & CR3_ADDR_MASK) &&
                      (loaded & CR3_PCID_MASK) == (pcid & CR3_PCID_MASK);
        if (!active)
            cpu_pcid_invalidate(cpu, (uint16_t)pcid);

        spinlock_release(&cpu->queue_lock);
        return active;
    }

    /* Without PCID every CR3 load flushes the previous address space's
     * non-global entries, so only a CPU with this exact pml4 in CR3 can have
     * user translations for it.  Skipping the rest is what makes a
     * single-threaded process's faults and unmaps IPI-free: before this,
     * every page invalidation interrupted every other core for nothing. */
    uint64_t loaded = __atomic_load_n(&cpu->active_cr3, __ATOMIC_ACQUIRE);
    if (loaded == 0)
        return true; /* tracking not published yet: be conservative */
    return (loaded & CR3_ADDR_MASK) == (pml4_base & CR3_ADDR_MASK);
}

#define KERNEL_IMAGE_BASE 0xFFFFFFFF80000000ULL

void tlb_shootdown_handle_ipi_regs(struct registers *regs) {
    struct cpu_info *self = cpu_get_current();
    if (!self)
        return;

    uint32_t id = self->cpu_id;
    if (id >= MAX_CPUS)
        return;

    uint64_t entry_tsc = rdtsc();
    uint64_t active_cr3;
    __asm__ volatile("mov %%cr3, %0" : "=r"(active_cr3));
    struct thread *current = self->current_thread;
    uint64_t current_tid = current ? current->tid : 0;
    uint64_t current_tgid = current ? current->tgid : 0;
    __atomic_store_n(&cpu_shootdown_entry_tsc[id], entry_tsc,
                     __ATOMIC_RELAXED);
    __atomic_store_n(&cpu_shootdown_seen_cr3[id], active_cr3,
                     __ATOMIC_RELAXED);
    __atomic_store_n(&cpu_shootdown_seen_tid[id], current_tid,
                     __ATOMIC_RELAXED);
    __atomic_store_n(&cpu_shootdown_seen_tgid[id], current_tgid,
                     __ATOMIC_RELAXED);

    if (current && current->comm[0]) {
        for (int c = 0; c < 15; c++) {
            cpu_shootdown_seen_comm[id][c] = current->comm[c];
            if (!current->comm[c]) break;
        }
        cpu_shootdown_seen_comm[id][15] = '\0';
    } else {
        cpu_shootdown_seen_comm[id][0] = '\0';
    }

    if (regs) {
        __atomic_store_n(&cpu_shootdown_interrupted_rip[id], regs->rip, __ATOMIC_RELAXED);
        __atomic_store_n(&cpu_shootdown_interrupted_cs[id], regs->cs, __ATOMIC_RELAXED);
        __atomic_store_n(&cpu_shootdown_interrupted_rflags[id], regs->rflags, __ATOMIC_RELAXED);
        __atomic_store_n(&cpu_shootdown_interrupted_rsp[id], regs->rsp, __ATOMIC_RELAXED);

        /* If interrupted in kernel mode, sample up to 4 return addresses from stack */
        if ((regs->cs & 3) == 0 && regs->rsp >= KERNEL_IMAGE_BASE && regs->rsp < 0xFFFFFFFFFF000000ULL) {
            uint64_t *sp = (uint64_t *)regs->rsp;
            int found = 0;
            for (int i = 0; i < 16 && found < 4; i++) {
                uint64_t val = sp[i];
                if (val >= KERNEL_IMAGE_BASE && val < 0xFFFFFFFFFF000000ULL) {
                    cpu_shootdown_kernel_stack[id][found++] = val;
                }
            }
            while (found < 4) {
                cpu_shootdown_kernel_stack[id][found++] = 0;
            }
        } else {
            for (int i = 0; i < 4; i++)
                cpu_shootdown_kernel_stack[id][i] = 0;
        }
    } else {
        __atomic_store_n(&cpu_shootdown_interrupted_rip[id], 0, __ATOMIC_RELAXED);
        __atomic_store_n(&cpu_shootdown_interrupted_cs[id], 0, __ATOMIC_RELAXED);
        __atomic_store_n(&cpu_shootdown_interrupted_rflags[id], 0, __ATOMIC_RELAXED);
        __atomic_store_n(&cpu_shootdown_interrupted_rsp[id], 0, __ATOMIC_RELAXED);
        for (int i = 0; i < 4; i++)
            cpu_shootdown_kernel_stack[id][i] = 0;
    }

    uint64_t addr = __atomic_load_n(&cpu_shootdown_addr[id], __ATOMIC_ACQUIRE);

    if (addr == TLB_SHOOTDOWN_ALL) {
        if (cpu_has_pcid()) {
            cpu_pcid_invalidate_all(self);
            pcid_flush_all();
        } else {
            uint64_t cr3;
            __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
            cr3 &= ~CR3_NOFLUSH;
            __asm__ volatile("mov %0, %%cr3" :: "r"(cr3) : "memory");
        }
    } else if (addr == TLB_SHOOTDOWN_CONTEXT) {
        uint16_t pcid = __atomic_load_n(&cpu_shootdown_pcid[id], __ATOMIC_ACQUIRE);
        if (!cpu_has_pcid() || pcid == PCID_KERNEL) {
            if (cpu_has_pcid()) {
                cpu_pcid_invalidate_all(self);
                pcid_flush_all();
            } else {
                uint64_t cr3;
                __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
                cr3 &= ~CR3_NOFLUSH;
                __asm__ volatile("mov %0, %%cr3" :: "r"(cr3) : "memory");
            }
            tlb_stat_add(&tlb_stats.handler_full_flushes, 1);
        } else {
            uint64_t cr3;
            __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
            uint16_t loaded = (uint16_t)(cr3 & CR3_PCID_MASK);
            if (loaded == pcid) {
                if (cpu_has_invpcid()) {
                    struct invpcid_desc desc = {0};
                    desc.pcid = pcid;
                    invpcid(INVPCID_TYPE_SINGLE_CTXT, &desc);
                } else {
                    cr3 &= ~CR3_NOFLUSH;
                    __asm__ volatile("mov %0, %%cr3" :: "r"(cr3) : "memory");
                }
            } else {
                cpu_pcid_invalidate(self, pcid);
            }
        }
    } else if (addr != 0) {
        /*
         * One page changed.  All we may throw away is the translation for that
         * page in the address space that owns it - which, without a PCID, means
         * we have to guess and nuke everything.  With a PCID we can be exact:
         *
         *   - The affected address space is the one loaded on *this* CPU: evict
         *     the single address.  invlpg/INVPCID are local and scoped to the
         *     current PCID, so nothing else is disturbed.
         *   - The affected address space is NOT loaded here: there is no way to
         *     reach its cached entries from this CPU, and there is no need.
         *     Clearing that PCID's bookkeeping bit makes the next switch into
         *     it reload CR3 with the flush bit set - which is exactly what the
         *     scheduler's CR3_NOFLUSH choice in sched/sched.c already asks for.
         *     The cost is deferred to the one core that actually goes back to
         *     that address space, instead of being charged to all of them now.
         *   - We were not told which address space (PCID_KERNEL): fall back to
         *     the old conservative full flush.
         */
        uint16_t pcid = __atomic_load_n(&cpu_shootdown_pcid[id], __ATOMIC_ACQUIRE);

        if (!cpu_has_pcid()) {
            tlb_stat_add(&tlb_stats.handler_full_flushes, 1);
            __asm__ volatile("invlpg (%0)" ::"r"(addr) : "memory");
        } else {
            uint64_t cr3;
            __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
            uint16_t loaded = (uint16_t)(cr3 & CR3_PCID_MASK);

            if (pcid == PCID_KERNEL) {
                tlb_stat_add(&tlb_stats.handler_full_flushes, 1);
                cpu_pcid_invalidate_all(self);
                if (cpu_has_invpcid()) {
                    struct invpcid_desc desc = {0};
                    invpcid(INVPCID_TYPE_ALL_NON_GLOBAL, &desc);
                } else {
                    cr3 &= ~CR3_NOFLUSH;
                    __asm__ volatile("mov %0, %%cr3" ::"r"(cr3) : "memory");
                }
            } else {
                if (pcid == loaded) {
                    if (cpu_has_invpcid()) {
                        struct invpcid_desc desc = {0};
                        desc.pcid = loaded;
                        desc.addr = addr;
                        invpcid(INVPCID_TYPE_INDIV_ADDR, &desc);
                    } else {
                        __asm__ volatile("invlpg (%0)" ::"r"(addr) : "memory");
                    }
                }
                /* Covers the "not loaded here" case, and is harmless (and
                 * redundant) in the loaded case only if entries for other
                 * addresses were touched - they were not, so this stays a
                 * single-bit clear when pcid == loaded is already handled. */
                if (pcid != loaded)
                    cpu_pcid_invalidate(self, pcid);
            }
        }
    }

    /* Ack: publish the completion snapshot before signaling the initiator. */
    __atomic_store_n(&cpu_shootdown_ack_tsc[id], rdtsc(), __ATOMIC_RELAXED);
    __atomic_store_n(&cpu_shootdown_ack[id], 0, __ATOMIC_RELEASE);
}

void tlb_shootdown_handle_ipi(void) {
    tlb_shootdown_handle_ipi_regs(NULL);
}

static void tlb_shootdown_isr(struct registers *regs) {
    tlb_shootdown_handle_ipi_regs(regs);
}

/* -------------------------------------------------------------------------
 * Acknowledgement watchdog
 *
 * The ack wait below is correct only while every target can actually take the
 * IPI.  A target with IF=0 - inside a critical section, or in a non-maskable
 * window - never runs the handler, and the old code spun forever: the machine
 * appeared to freeze with a completely idle serial line and no hint of which
 * CPU was holding everyone up.  Ten seconds is orders of magnitude beyond the
 * few microseconds a healthy CPU needs to acknowledge, so exceeding it means
 * the protocol is broken.  Naming the stuck CPU (and the thread it is sitting
 * in) is worth far more than an unexplained hang.
 * ------------------------------------------------------------------------- */
#define SHOOTDOWN_TIMEOUT_MS 10000ULL

/* How many LOCKDEP notes to print before going quiet about a repeated
 * violation.  See the note in do_shootdown() for why each one is not free. */
#define LOCKDEP_NOTE_LIMIT 6

/* Everything below goes out through serial_write_sync(), which writes straight
 * to the UART with no lock.  klog_puts() would take klog_lock and then
 * serial_lock - and a CPU sitting in a critical section that is part of the
 * very deadlock being reported may be holding exactly those, which would turn
 * "here is the stuck CPU" into "the log stops here". */
static void sd_note(const char *s) {
  size_t n = 0;
  while (s[n])
    n++;
  serial_write_sync(s, n);
}

static void sd_dec(uint64_t v) {
  char buf[24];
  int i = (int)sizeof(buf);
  do {
    buf[--i] = (char)('0' + (v % 10));
    v /= 10;
  } while (v != 0 && i > 0);
  serial_write_sync(&buf[i], (size_t)(sizeof(buf) - (size_t)i));
}

static void sd_hex(uint64_t v) {
  static const char hex[] = "0123456789ABCDEF";
  char buf[19];
  buf[0] = '0';
  buf[1] = 'x';
  for (unsigned i = 0; i < 16; i++)
    buf[2 + i] = hex[(v >> (((15 - i) * 4))) & 0xF];
  buf[18] = '\0';
  serial_write_sync(buf, 18);
}

static void shootdown_stuck(struct cpu_info *self, struct cpu_info *victim,
                            uint64_t addr) {
    kpf_dump_panic_entry("TLB shootdown acknowledgement timeout", NULL);

    sd_note("\n[TLB] FATAL: shootdown timeout addr=");
    sd_hex(addr);
    sd_note(" initiator_cpu=");
    sd_dec(self ? self->cpu_id : 0xFFFFFFFFFFFFFFFFULL);
    sd_note(" waiting_on_cpu=");
    sd_dec(victim ? victim->cpu_id : 0xFFFFFFFFFFFFFFFFULL);
    sd_note(" status=");
    sd_dec(victim ? victim->status : 0xFFFFFFFFFFFFFFFFULL);
    sd_note(" apic=");
    sd_dec(victim ? victim->apic_id : 0xFFFFFFFFFFFFFFFFULL);
    sd_note("\n[TLB]   stuck thread tid=");
    if (victim && victim->current_thread) {
        sd_dec(victim->current_thread->tid);
        sd_note(" comm='");
        if (victim->current_thread->comm[0])
            sd_note(victim->current_thread->comm);
        sd_note("'\n");
    } else {
        sd_note(" (no thread on that CPU)\n");
    }
    sd_note("[TLB]   Someone holds a lock across a shootdown, or a CPU was left"
            " with interrupts disabled so it cannot answer the IPI."
            " See mm/tlb_shootdown.c and lock/spinlock.h.\n");

    if (self && lockdiag_self_holds_vmm_lock(self->cpu_id))
        sd_note("[TLB]   NOTE: the initiator itself holds vmm_lock right now."
                " A target takes its page fault handler through vmm_lock, and a"
                " fault handler cannot take an IPI, so that acknowledgement can"
                " never arrive: this is a deadlock, not a slow CPU.\n");

    /* Which cores can still take interrupts, and what every core is executing,
     * is what identifies the culprit; the narrow report above only names the
     * core we happened to be waiting for. */
    lockdiag_dump_all("TLB shootdown acknowledgement timeout");

    for (;;)
        __asm__ volatile("cli; hlt");
}

void tlb_shootdown_init(void) {
    for (int i = 0; i < MAX_CPUS; i++) {
        cpu_shootdown_addr[i] = 0;
        cpu_shootdown_pcid[i] = PCID_KERNEL;
        cpu_shootdown_ack[i]  = 0;
        cpu_shootdown_sent_tsc[i] = 0;
        cpu_shootdown_entry_tsc[i] = 0;
        cpu_shootdown_ack_tsc[i] = 0;
        cpu_shootdown_seen_cr3[i] = 0;
        cpu_shootdown_seen_tid[i] = 0;
    }
    register_interrupt_handler(IPI_VECTOR_TLB_SHOOTDOWN, tlb_shootdown_isr);
    klog_puts("[TLB] Shootdown IPI handler registered on vector 0x");
    klog_hex32(IPI_VECTOR_TLB_SHOOTDOWN);
    klog_puts("\n");
}

/* -------------------------------------------------------------------------
 * do_shootdown — core implementation
 *
 * 1. Determine which remote CPUs need the shootdown (CR3 filter).
 * 2. Write the address into each target's per-CPU slot.
 * 3. Flush locally (overlap with the IPI round-trip).
 * 4. Send IPIs to all targets in one burst.
 * 5. Spin until all targets ack (they should all be finishing concurrently).
 * ------------------------------------------------------------------------- */
/* PCID currently loaded on this CPU, or PCID_KERNEL when PCID is unused. */
static uint16_t local_pcid(void) {
    if (!cpu_has_pcid())
        return PCID_KERNEL;
    uint64_t cr3;
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
    return (uint16_t)(cr3 & CR3_PCID_MASK);
}

/* Which PCID's cached translations cover @pml4, as seen from this CPU: the
 * loaded PCID when that address space is the active one, else the
 * conservative PCID_KERNEL (every target flushes everything rather than
 * risking a stale entry under the wrong tag). */
static uint16_t pcid_for_pml4(uint64_t pml4) {
    if (!cpu_has_pcid())
        return PCID_KERNEL;
    uint64_t cr3;
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
    if ((cr3 & CR3_ADDR_MASK) == (pml4 & CR3_ADDR_MASK))
        return (uint16_t)(cr3 & CR3_PCID_MASK);
    return PCID_KERNEL;
}

/*
 * Invalidate on this CPU.  With a known PCID the affected address space is the
 * one loaded here (see tlb_shootdown_page_for), so a single invlpg suffices.
 * Without one we cannot tell which of this core's PCID-tagged entries are
 * stale, so fall back to a full flush rather than risk keeping one alive.
 */
static void local_flush(uint64_t addr, uint16_t pcid) {
    if (addr == TLB_SHOOTDOWN_ALL) {
        if (cpu_has_pcid()) {
            cpu_pcid_invalidate_all(cpu_get_current());
            pcid_flush_all();
        } else {
            uint64_t cr3;
            __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
            cr3 &= ~CR3_NOFLUSH;
            __asm__ volatile("mov %0, %%cr3" :: "r"(cr3) : "memory");
        }
        return;
    }

    if (addr == TLB_SHOOTDOWN_CONTEXT) {
        if (!cpu_has_pcid() || pcid == PCID_KERNEL) {
            if (cpu_has_pcid()) {
                cpu_pcid_invalidate_all(cpu_get_current());
                pcid_flush_all();
            } else {
                uint64_t cr3;
                __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
                cr3 &= ~CR3_NOFLUSH;
                __asm__ volatile("mov %0, %%cr3" :: "r"(cr3) : "memory");
            }
        } else if (cpu_has_invpcid()) {
            struct invpcid_desc desc = {0};
            desc.pcid = pcid;
            invpcid(INVPCID_TYPE_SINGLE_CTXT, &desc);
        } else {
            uint64_t cr3;
            __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
            if ((cr3 & CR3_PCID_MASK) == pcid) {
                cr3 &= ~CR3_NOFLUSH;
                __asm__ volatile("mov %0, %%cr3" :: "r"(cr3) : "memory");
            } else
                cpu_pcid_invalidate(cpu_get_current(), pcid);
        }
        return;
    }

    if (pcid == PCID_KERNEL && cpu_has_pcid()) {
        tlb_stat_add(&tlb_stats.handler_full_flushes, 1);
        cpu_pcid_invalidate_all(cpu_get_current());
        pcid_flush_all();
        return;
    }

    __asm__ volatile("invlpg (%0)" :: "r"(addr) : "memory");
}

/* --- who is issuing shootdowns ------------------------------------------
 * 2079 shootdowns at boot, every one of them started while holding vmm_lock,
 * is only actionable if the report says which call sites they came from.
 * Fixed table, no allocation, one linear probe per shootdown. */
#define SHOOTDOWN_CALLER_SLOTS 8
typedef struct {
    uint64_t ip;
    volatile uint64_t total;
    volatile uint64_t under_vmm_lock;
} sd_caller_t;

static sd_caller_t sd_callers[SHOOTDOWN_CALLER_SLOTS];

static void sd_note_caller(uint64_t ip, bool under_lock) {
    if (!ip)
        return;
    sd_caller_t *free_slot = NULL;
    for (int i = 0; i < SHOOTDOWN_CALLER_SLOTS; i++) {
        if (sd_callers[i].ip == ip) {
            __atomic_add_fetch(&sd_callers[i].total, 1, __ATOMIC_RELAXED);
            if (under_lock)
                __atomic_add_fetch(&sd_callers[i].under_vmm_lock, 1,
                                   __ATOMIC_RELAXED);
            return;
        }
        if (!free_slot && sd_callers[i].ip == 0)
            free_slot = &sd_callers[i];
    }
    if (free_slot) {
        free_slot->ip = ip;
        free_slot->total = 1;
        free_slot->under_vmm_lock = under_lock ? 1 : 0;
    }
}

/* Read side for the lockdiag report: slot i, or 0 when past the end. */
uint64_t tlb_shootdown_caller_info(int slot, uint64_t *ip, uint64_t *total,
                                   uint64_t *under_vmm_lock) {
    if (slot < 0 || slot >= SHOOTDOWN_CALLER_SLOTS)
        return 0;
    if (sd_callers[slot].ip == 0)
        return 0;
    if (ip)
        *ip = sd_callers[slot].ip;
    if (total)
        *total = __atomic_load_n(&sd_callers[slot].total, __ATOMIC_RELAXED);
    if (under_vmm_lock)
        *under_vmm_lock =
            __atomic_load_n(&sd_callers[slot].under_vmm_lock, __ATOMIC_RELAXED);
    return 1;
}

/* --- deferred (non-blocking) invalidation requests ----------------------
 *
 * A shootdown that waits for acknowledgements must never be issued while the
 * initiator holds a lock that its targets also take: every remote core reaches
 * its page fault handler through vmm_lock, and a fault handler cannot take an
 * IPI (the IDT uses interrupt gates), so a target that faults while we wait
 * cannot acknowledge and both cores spin forever.  mm/vmm_map.c therefore
 * *requests* invalidations while it holds vmm_lock and drains them from
 * vmm_lock_release(), once the lock is gone.
 *
 * Requests are per-CPU and need no lock: only their own CPU appends or drains
 * them.  The IRQ guard covers the case where a fault on this same CPU re-enters
 * the paging engine and drains the list underneath us; draining early is always
 * safe, losing an append never is.  A full list of user mappings for one root
 * collapses into a context invalidation; mixed roots or kernel mappings retain
 * the conservative global fallback. */
#define TLB_DEFER_SLOTS 32
#define TLB_MAX_CPUS 64

typedef struct {
  uint64_t addr;
  uint64_t pml4;
} tlb_pending_t;

static tlb_pending_t tlb_pending[TLB_MAX_CPUS][TLB_DEFER_SLOTS];
static volatile uint32_t tlb_pending_count[TLB_MAX_CPUS];
static volatile uint8_t tlb_pending_all[TLB_MAX_CPUS];

static int tlb_self_slot(void) {
  struct cpu_info *c = cpu_get_current();
  if (!c || c->cpu_id >= TLB_MAX_CPUS)
    return -1;
  return (int)c->cpu_id;
}

void tlb_flush_deferred(uint64_t addr, uint64_t pml4) {
  int slot = tlb_self_slot();
  if (slot < 0) {
    /* No per-CPU area yet (early boot): a synchronous flush is safe here
     * because nothing else is running to fault against us. */
    tlb_shootdown_page_for(addr, pml4);
    return;
  }

  hal_irq_state_t flags = hal_irq_save();
  if (__atomic_load_n(&tlb_pending_all[slot], __ATOMIC_RELAXED)) {
    hal_irq_restore(flags);
    return; /* already collapsed */
  }
  uint32_t n = __atomic_load_n(&tlb_pending_count[slot], __ATOMIC_RELAXED);

  /* A context invalidation subsumes every user-page invalidation for that
   * same page-table root. Coalesce it before the bounded queue can overflow. */
  uint64_t root = pml4 & CR3_ADDR_MASK;
  bool user_scoped = addr == TLB_SHOOTDOWN_CONTEXT ||
                     (addr != 0 && addr <= USER_SPACE_LIMIT);
  if (root && user_scoped) {
    for (uint32_t i = 0; i < n; i++) {
      uint64_t queued_addr = tlb_pending[slot][i].addr;
      uint64_t queued_root = tlb_pending[slot][i].pml4 & CR3_ADDR_MASK;
      if (queued_root == root && queued_addr == TLB_SHOOTDOWN_CONTEXT) {
        hal_irq_restore(flags);
        return;
      }
    }

    if (addr == TLB_SHOOTDOWN_CONTEXT) {
      uint32_t out = 0;
      for (uint32_t i = 0; i < n; i++) {
        uint64_t queued_addr = tlb_pending[slot][i].addr;
        uint64_t queued_root = tlb_pending[slot][i].pml4 & CR3_ADDR_MASK;
        bool same_user_root = queued_root == root && queued_addr != 0 &&
                              queued_addr <= USER_SPACE_LIMIT;
        if (!same_user_root)
          tlb_pending[slot][out++] = tlb_pending[slot][i];
      }
      n = out;
      __atomic_store_n(&tlb_pending_count[slot], n, __ATOMIC_RELAXED);
    }
  }

  if (n >= TLB_DEFER_SLOTS) {
    /* A burst of unmaps from one process needs one context flush, not a
     * machine-wide broadcast. Retain the global fallback only for mixed roots
     * or kernel mappings, where the affected translations cannot be scoped. */
    bool collapse_context = root && user_scoped;
    for (uint32_t i = 0; collapse_context && i < n; i++) {
      uint64_t queued_addr = tlb_pending[slot][i].addr;
      uint64_t queued_root = tlb_pending[slot][i].pml4 & CR3_ADDR_MASK;
      bool queued_user = queued_addr == TLB_SHOOTDOWN_CONTEXT ||
                         (queued_addr != 0 &&
                          queued_addr <= USER_SPACE_LIMIT);
      if (queued_root != root || !queued_user)
        collapse_context = false;
    }
    if (collapse_context) {
      tlb_pending[slot][0].addr = TLB_SHOOTDOWN_CONTEXT;
      tlb_pending[slot][0].pml4 = root;
      __atomic_store_n(&tlb_pending_count[slot], 1, __ATOMIC_RELAXED);
    } else {
      __atomic_store_n(&tlb_pending_all[slot], 1, __ATOMIC_RELAXED);
      __atomic_store_n(&tlb_pending_count[slot], 0, __ATOMIC_RELAXED);
    }
    hal_irq_restore(flags);
    return;
  }
  tlb_pending[slot][n].addr = addr;
  tlb_pending[slot][n].pml4 = pml4;
  __atomic_store_n(&tlb_pending_count[slot], n + 1, __ATOMIC_RELAXED);
  hal_irq_restore(flags);
}

void tlb_flush_deferred_context(uint64_t pml4) {
  tlb_flush_deferred(TLB_SHOOTDOWN_CONTEXT, pml4);
}

void tlb_flush_deferred_all(void) {
  int slot = tlb_self_slot();
  if (slot < 0) {
    tlb_shootdown_all();
    return;
  }
  hal_irq_state_t flags = hal_irq_save();
  __atomic_store_n(&tlb_pending_all[slot], 1, __ATOMIC_RELAXED);
  __atomic_store_n(&tlb_pending_count[slot], 0, __ATOMIC_RELAXED);
  hal_irq_restore(flags);
}

bool tlb_flush_deferred_drain(void) {
  int slot = tlb_self_slot();
  if (slot < 0)
    return true;

  hal_irq_state_t flags = hal_irq_save();
  uint8_t all = __atomic_load_n(&tlb_pending_all[slot], __ATOMIC_RELAXED);
  uint32_t n = __atomic_load_n(&tlb_pending_count[slot], __ATOMIC_RELAXED);
  if (!all && n == 0) {
    hal_irq_restore(flags);
    return true;
  }

  if (!(flags & SPINLOCK_RFLAGS_IF)) {
    /* Interrupts are masked: waiting for remote acknowledgements here would
     * deadlock against a target sitting in another masked section (a page
     * fault handler, for instance), because such a target can never run the
     * IPI handler.  Do what this CPU needs immediately - the local
     * invalidations - and leave the remote part queued for a drain that can
     * wait.  Returning false tells the caller that page-table frames must
     * stay parked: a remote CPU may still be walking them. */
    hal_irq_restore(flags);
    if (all) {
      local_flush(TLB_SHOOTDOWN_ALL, PCID_KERNEL);
    } else {
      for (uint32_t i = 0; i < n; i++)
        local_flush(tlb_pending[slot][i].addr,
                    pcid_for_pml4(tlb_pending[slot][i].pml4));
    }
    return false;
  }

  /* Take a stable snapshot before reopening interrupts. New invalidations
   * queued while this batch waits for remote CPUs belong to the next drain;
   * they must not overwrite entries this drain is still reading. */
  tlb_pending_t requests[TLB_DEFER_SLOTS];
  for (uint32_t i = 0; i < n; i++)
    requests[i] = tlb_pending[slot][i];
  __atomic_store_n(&tlb_pending_all[slot], 0, __ATOMIC_RELAXED);
  __atomic_store_n(&tlb_pending_count[slot], 0, __ATOMIC_RELAXED);
  hal_irq_restore(flags);

  /* A context flush subsumes every page flush for that user address space.
   * Grouping a burst of unmaps this way avoids serially taking shootdown_lock
   * and waiting for the same CPUs once per page. */
  if (all) {
    tlb_shootdown_all();
    return true;
  }
  bool processed[TLB_DEFER_SLOTS] = {false};
  for (uint32_t i = 0; i < n; i++)
    if (!processed[i]) {
      uint64_t addr = requests[i].addr;
      uint64_t root = requests[i].pml4 & CR3_ADDR_MASK;
      bool user_page = addr != 0 && addr <= USER_SPACE_LIMIT;
      if (root && (addr == TLB_SHOOTDOWN_CONTEXT || user_page)) {
        uint32_t same_root_pages = 0;
        bool has_context = addr == TLB_SHOOTDOWN_CONTEXT;
        for (uint32_t j = i; j < n; j++) {
          uint64_t other_root = requests[j].pml4 & CR3_ADDR_MASK;
          uint64_t other_addr = requests[j].addr;
          if (other_root != root)
            continue;
          if (other_addr == TLB_SHOOTDOWN_CONTEXT) {
            has_context = true;
          } else if (other_addr != 0 && other_addr <= USER_SPACE_LIMIT) {
            same_root_pages++;
          }
        }
        if (has_context || same_root_pages > 1) {
          for (uint32_t j = i; j < n; j++) {
            uint64_t other_root = requests[j].pml4 & CR3_ADDR_MASK;
            uint64_t other_addr = requests[j].addr;
            bool other_user = other_addr == TLB_SHOOTDOWN_CONTEXT ||
                              (other_addr != 0 &&
                               other_addr <= USER_SPACE_LIMIT);
            if (other_root == root && other_user)
              processed[j] = true;
          }
          tlb_shootdown_context_for(root);
        } else {
          processed[i] = true;
          tlb_shootdown_page_for(addr, requests[i].pml4);
        }
      } else if (addr == TLB_SHOOTDOWN_CONTEXT) {
        tlb_shootdown_context_for(requests[i].pml4);
      } else {
        tlb_shootdown_page_for(addr, requests[i].pml4);
      }
    }
  return true;
}

static void do_shootdown(uint64_t addr, uint16_t pcid, uint64_t pml4_base,
                        uint64_t caller_ip) {
    uint32_t cpu_count = cpu_get_count();
    struct cpu_info *self = cpu_get_current();

    /* Freeze the target set before anything is published: the predicate reads
     * live per-CPU state, and the publish/send/wait phases must all agree on
     * exactly which CPUs were told to invalidate. */
    uint64_t target_mask = 0;
    uint32_t targets = 0;
    for (uint32_t i = 0; i < cpu_count && i < MAX_CPUS; i++) {
        struct cpu_info *c = cpu_get_info(i);
        if (!cpu_needs_shootdown(c, self, addr, pcid, pml4_base))
            continue;
        target_mask |= 1ULL << c->cpu_id;
        targets++;
    }

    if (targets == 0) {
        local_flush(addr, pcid);
        return;
    }

    /* A caller with interrupts masked cannot wait for acknowledgements.  A
     * target that is itself in an interrupt-masked section (the page-fault
     * gate enters with IF=0 and its handler runs under it) can never run the
     * shootdown IPI handler, so the ack cannot arrive: the initiator would
     * spin here forever while the target spins behind a lock this path never
     * releases.  Invalidate locally - which is what this CPU needs before it
     * re-executes the faulting access - and record the remote part for the
     * next drain that can wait (see tlb_flush_deferred_drain()). */
    if (!hal_irq_enabled()) {
        local_flush(addr, pcid);
        if (tlb_self_slot() >= 0) {
            if (addr == TLB_SHOOTDOWN_ALL)
                tlb_flush_deferred_all();
            else
                tlb_flush_deferred(addr, pml4_base);
        }
        return;
    }

    spinlock_acquire(&shootdown_lock);

    LOCKDIAG_STAT(shootdowns, 1);
    tlb_stat_add(&tlb_stats.shootdowns, 1);
    tlb_stat_add(&tlb_stats.ipis_sent, targets);
    if (addr == TLB_SHOOTDOWN_ALL)
        tlb_stat_add(&tlb_stats.broadcast_all, 1);

    /* Publish address to all target CPUs. */
    for (uint32_t i = 0; i < cpu_count && i < MAX_CPUS; i++) {
        struct cpu_info *c = cpu_get_info(i);
        if (!(target_mask & (1ULL << c->cpu_id)))
            continue;
        __atomic_store_n(&cpu_shootdown_pcid[c->cpu_id], pcid, __ATOMIC_RELEASE);
        __atomic_store_n(&cpu_shootdown_addr[c->cpu_id], addr, __ATOMIC_RELEASE);
        __atomic_store_n(&cpu_shootdown_sent_tsc[c->cpu_id], 0, __ATOMIC_RELAXED);
        __atomic_store_n(&cpu_shootdown_entry_tsc[c->cpu_id], 0, __ATOMIC_RELAXED);
        __atomic_store_n(&cpu_shootdown_ack_tsc[c->cpu_id], 0, __ATOMIC_RELAXED);
        __atomic_store_n(&cpu_shootdown_seen_cr3[c->cpu_id], 0, __ATOMIC_RELAXED);
        __atomic_store_n(&cpu_shootdown_seen_tid[c->cpu_id], 0, __ATOMIC_RELAXED);
        __atomic_store_n(&cpu_shootdown_ack[c->cpu_id], 1, __ATOMIC_RELEASE);
    }

    /* Local flush first - overlaps with IPI delivery latency. */
    local_flush(addr, pcid);

    /* Send all IPIs in one burst. */
    for (uint32_t i = 0; i < cpu_count && i < MAX_CPUS; i++) {
        struct cpu_info *c = cpu_get_info(i);
        if (!(target_mask & (1ULL << c->cpu_id)))
            continue;
        __atomic_store_n(&cpu_shootdown_sent_tsc[c->cpu_id], rdtsc(),
                         __ATOMIC_RELAXED);
        lapic_send_ipi(c->apic_id, IPI_VECTOR_TLB_SHOOTDOWN);
    }

    /* Rule check, deliberately printed BEFORE the wait instead of only after it
     * times out: when this is the real deadlock the machine may never print
     * again, and "CPU N started a shootdown while holding vmm_lock" is the
     * entire answer.  Every target reaches its page fault handler through
     * vmm_lock, and a fault handler cannot take an IPI (the IDT uses interrupt
     * gates), so a target that faults while we wait can never acknowledge.
     *
     * Rate limited: each note is ~150 bytes written straight to the UART, which
     * is ~13 ms at 115200 baud spent inside vmm_lock, and drivers that re-map
     * whole BARs hit this once per page.  The counter keeps counting whatever
     * the log stops showing, and it is printed by the hang report. */
    bool under_vmm_lock = self && lockdiag_self_holds_vmm_lock(self->cpu_id);
    sd_note_caller(caller_ip, under_vmm_lock);
    if (under_vmm_lock) {
        LOCKDIAG_STAT(shootdown_under_vmm_lock, 1);
        static volatile uint32_t lockdep_notes;
        uint32_t seen =
            __atomic_add_fetch(&lockdep_notes, 1, __ATOMIC_RELAXED);
        if (seen <= LOCKDEP_NOTE_LIMIT) {
            sd_note("\n[TLB] LOCKDEP: shootdown started while holding vmm_lock"
                    " initiator_cpu=");
            sd_dec(self->cpu_id);
            sd_note(" addr=");
            sd_hex(addr);
            sd_note(" targets=");
            sd_dec(targets);
            sd_note(" — a target that faults on vmm_lock cannot acknowledge"
                    " this IPI. See mm/vmm_map.c and mm/vmm_clone.c.\n");
        }
        if (seen == LOCKDEP_NOTE_LIMIT)
            sd_note("[TLB] LOCKDEP: further notes suppressed (still counted as"
                    " 'shot-down-under-vmm_lock' in the hang report).\n");
    }

    /* Wait for all acks, with a watchdog (see shootdown_stuck).  rdtsc() is
     * only meaningful once the TSC is calibrated; early boot falls back to a
     * plain iteration bound so this cannot false-positive at startup. */
    uint64_t mhz = tsc_get_mhz();
    /* mhz is TSC ticks per MICROSECOND, so a millisecond is mhz*1000 ticks.
     * Multiplying by mhz alone (as this used to) turned this "10 s" watchdog
     * into a 10 ms one, which under load can fire on a merely slow - not stuck -
     * core and halt every CPU in the machine over nothing. */
    uint64_t deadline =
        mhz ? rdtsc() + mhz * 1000ULL * SHOOTDOWN_TIMEOUT_MS : 0;
    uint64_t fallback_spins = 400000000ULL;
    uint64_t wait_start = rdtsc();
    uint64_t ack_observed_tsc[MAX_CPUS] = {0};
    uint64_t pending_mask = target_mask;
    while (pending_mask) {
        bool progress = false;
        for (uint32_t i = 0; i < cpu_count && i < MAX_CPUS; i++) {
            struct cpu_info *c = cpu_get_info(i);
            if (!c || !(pending_mask & (1ULL << c->cpu_id)))
                continue;
            if (__atomic_load_n(&cpu_shootdown_ack[c->cpu_id],
                                __ATOMIC_ACQUIRE) == 0) {
                /* This timestamp shares the initiator's TSC with sent_tsc, so
                 * ack_us below does not depend on cross-core TSC alignment. */
                ack_observed_tsc[c->cpu_id] = rdtsc();
                pending_mask &= ~(1ULL << c->cpu_id);
                progress = true;
            }
        }
        if (!pending_mask)
            break;

        if (deadline) {
            if (rdtsc() > deadline) {
                for (uint32_t i = 0; i < cpu_count && i < MAX_CPUS; i++) {
                    struct cpu_info *c = cpu_get_info(i);
                    if (c && (pending_mask & (1ULL << c->cpu_id)))
                        shootdown_stuck(self, c, addr);
                }
            }
        } else if (--fallback_spins == 0) {
            for (uint32_t i = 0; i < cpu_count && i < MAX_CPUS; i++) {
                struct cpu_info *c = cpu_get_info(i);
                if (c && (pending_mask & (1ULL << c->cpu_id)))
                    shootdown_stuck(self, c, addr);
            }
        }
        if (!progress)
            hal_cpu_relax();
    }

    uint64_t waited = rdtsc() - wait_start;
    if (mhz) {
        tlb_stat_add(&tlb_stats.ack_wait_ns, waited / mhz);
        tlb_stat_max(&tlb_stats.max_ack_wait_ns, waited / mhz);

        /* mhz is TSC ticks per microsecond. */
        uint64_t waited_ms = waited / (mhz * 1000ULL);
        LOCKDIAG_STAT_MAX(shootdown_max_ack_ns,
                          tsc_cycles_to_ns(waited));
        if (waited_ms > LOCKDIAG_SLOW_ACK_MS) {
            LOCKDIAG_STAT(shootdown_slow_acks, 1);
            uint32_t slow_seen = __atomic_add_fetch(&slow_ack_log_count, 1,
                                                    __ATOMIC_RELAXED);
            /* A single busy process can generate thousands of page shootdowns.
             * Keep the initial breakdown, then sample periodically; the full
             * count and worst latency remain available in lockdiag stats. */
            bool emit_detail = slow_seen <= 32 || (slow_seen & 0x1Fu) == 0;
            if (slow_seen == 33) {
                sd_note("[TLB] further SLOW ack breakdowns suppressed; "
                        "sampling every 32nd event (counter remains in stats)\n");
            }
            if (emit_detail) {
            sd_note("\n[TLB] SLOW ack wait=");
            sd_dec(waited_ms);
            sd_note("ms/");
            sd_dec(waited / mhz);
            sd_note("us kind=");
            sd_note(addr == TLB_SHOOTDOWN_ALL ? "all" :
                    addr == TLB_SHOOTDOWN_CONTEXT ? "context" : "page");
            sd_note(" addr=");
            sd_hex(addr);
            sd_note(" pml4=");
            sd_hex(pml4_base);
            sd_note(" pcid=");
            sd_hex(pcid);
            sd_note(" targets=");
            sd_dec(targets);
            sd_note(" mask=");
            sd_hex(target_mask);
            sd_note(" initiator_cpu=");
            sd_dec(self ? self->cpu_id : 0xFFFFFFFFFFFFFFFFULL);
            sd_note(" caller=");
            sd_hex(caller_ip);
            sd_note("\n");

            /* The aggregate only says that at least one target was late.
             * Break it down per core so an IPI delivery delay can be separated
             * from time spent executing the invalidation handler. */
            for (uint32_t i = 0; i < cpu_count && i < MAX_CPUS; i++) {
                struct cpu_info *c = cpu_get_info(i);
                if (!c || !(target_mask & (1ULL << c->cpu_id)))
                    continue;

                uint32_t id = c->cpu_id;
                uint64_t sent = __atomic_load_n(&cpu_shootdown_sent_tsc[id],
                                                __ATOMIC_RELAXED);
                uint64_t entry = __atomic_load_n(&cpu_shootdown_entry_tsc[id],
                                                 __ATOMIC_RELAXED);
                uint64_t ack = __atomic_load_n(&cpu_shootdown_ack_tsc[id],
                                               __ATOMIC_RELAXED);
                uint64_t seen_cr3 = __atomic_load_n(&cpu_shootdown_seen_cr3[id],
                                                    __ATOMIC_RELAXED);
                uint64_t seen_tid = __atomic_load_n(&cpu_shootdown_seen_tid[id],
                                                    __ATOMIC_RELAXED);
                uint64_t seen_tgid = __atomic_load_n(&cpu_shootdown_seen_tgid[id],
                                                     __ATOMIC_RELAXED);
                uint64_t ack_seen = ack_observed_tsc[id];
                uint64_t ack_us = sent && ack_seen >= sent
                                      ? (ack_seen - sent) / mhz : 0;
                uint64_t delivery_us = sent && entry >= sent
                                           ? (entry - sent) / mhz : 0;
                uint64_t handler_us = entry && ack >= entry ? (ack - entry) / mhz : 0;
                uint64_t rtt_us = ack && ack_seen >= ack ? (ack_seen - ack) / mhz : 0;

                uint64_t rip = __atomic_load_n(&cpu_shootdown_interrupted_rip[id], __ATOMIC_RELAXED);
                uint64_t cs = __atomic_load_n(&cpu_shootdown_interrupted_cs[id], __ATOMIC_RELAXED);
                uint64_t rflags = __atomic_load_n(&cpu_shootdown_interrupted_rflags[id], __ATOMIC_RELAXED);
                uint64_t rsp = __atomic_load_n(&cpu_shootdown_interrupted_rsp[id], __ATOMIC_RELAXED);

                sd_note("[TLB]   cpu=");
                sd_dec(id);
                sd_note(" apic=");
                sd_dec(c->apic_id);
                sd_note(" ack_us=");
                sd_dec(ack_us);
                sd_note(" (delivery=");
                sd_dec(delivery_us);
                sd_note("us handler=");
                sd_dec(handler_us);
                sd_note("us rtt=");
                sd_dec(rtt_us);
                sd_note("us) cr3=");
                sd_hex(seen_cr3);
                sd_note(" tid=");
                sd_dec(seen_tid);
                sd_note(" tgid=");
                sd_dec(seen_tgid);
                if (cpu_shootdown_seen_comm[id][0]) {
                    sd_note(" comm='");
                    sd_note(cpu_shootdown_seen_comm[id]);
                    sd_note("'");
                }
                sd_note("\n");

                sd_note("[TLB]     interrupted mode=");
                if ((cs & 3) == 3) {
                    sd_note("USER");
                } else if (cs != 0) {
                    sd_note("KERNEL");
                } else {
                    sd_note("UNKNOWN");
                }
                sd_note(" rip=");
                sd_hex(rip);
                sd_note(" cs=");
                sd_hex(cs);
                sd_note(" rflags=");
                sd_hex(rflags);
                sd_note(" rsp=");
                sd_hex(rsp);
                sd_note("\n");

                if ((cs & 3) == 0 && rip != 0) {
                    bool has_stack = false;
                    for (int k = 0; k < 4; k++) {
                        if (cpu_shootdown_kernel_stack[id][k]) {
                            has_stack = true;
                            break;
                        }
                    }
                    if (has_stack) {
                        sd_note("[TLB]     stack:");
                        for (int k = 0; k < 4; k++) {
                            if (cpu_shootdown_kernel_stack[id][k]) {
                                sd_note(" ");
                                sd_hex(cpu_shootdown_kernel_stack[id][k]);
                            }
                        }
                        sd_note("\n");
                    }
                }

                uint64_t w_lock = 0, w_ip = 0, w_since = 0;
                if (lockdiag_get_spin_wait(id, &w_lock, &w_ip, &w_since)) {
                    sd_note("[TLB]     WAITING-ON lock=");
                    sd_hex(w_lock);
                    sd_note(" at=");
                    sd_hex(w_ip);
                    if (w_since && mhz && rdtsc() > w_since) {
                        sd_note(" for=");
                        sd_dec((rdtsc() - w_since) / (mhz * 1000ULL));
                        sd_note("ms");
                    }
                    sd_note("\n");
                }

                for (int s = 0; s < LOCKDIAG_SPOT_COUNT; s++) {
                    lockdiag_spot_t *sp = lockdiag_spot(s);
                    if (sp && sp->depth && sp->owner_cpu == id) {
                        sd_note("[TLB]     HOLDS ");
                        sd_note(lockdiag_spot_name(s));
                        sd_note(" taken_at=");
                        sd_hex(sp->taken_ip);
                        sd_note("\n");
                    }
                }
            }
            }
        }
    }

    spinlock_release(&shootdown_lock);
}

/* This CPU's loaded pml4 physical address. */
static uint64_t local_pml4(void) {
    uint64_t cr3;
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
    return cr3 & CR3_ADDR_MASK;
}

void tlb_shootdown_page(uint64_t addr) {
    /*
     * Assumes the affected address space is the one loaded on this CPU, which
     * is true for the fault path and for every syscall touching its own mm.
     * Anything manipulating another address space must use
     * tlb_shootdown_page_for(), or remote CPUs will be told to flush the wrong
     * PCID.
     */
    do_shootdown(addr, local_pcid(), local_pml4(),
                 (uint64_t)__builtin_return_address(0));
}

void tlb_shootdown_page_for(uint64_t addr, uint64_t pml4) {
    /* When the address space is not the one loaded here the caller is tearing
     * down an address space that is not loaded on this CPU, and we genuinely
     * do not know which PCID owns the stale entries: pcid_for_pml4() returns
     * PCID_KERNEL and every target behaves conservatively. */
    do_shootdown(addr, pcid_for_pml4(pml4), pml4 & CR3_ADDR_MASK,
                 (uint64_t)__builtin_return_address(0));
}

void tlb_shootdown_context_for(uint64_t pml4) {
    do_shootdown(TLB_SHOOTDOWN_CONTEXT, pcid_for_pml4(pml4),
                 pml4 & CR3_ADDR_MASK,
                 (uint64_t)__builtin_return_address(0));
}

void tlb_shootdown_all(void) {
    do_shootdown(TLB_SHOOTDOWN_ALL, PCID_KERNEL, 0,
                 (uint64_t)__builtin_return_address(0));
}

/* ── `tlb_bench=1`: single-page shootdown cost ───────────────────────────────
 *
 * The dominant cost of a page invalidation is the remote part: every online
 * CPU is interrupted and must acknowledge, whether or not it ever ran the
 * address space being modified.  This measures the two shapes on the real
 * hardware path, against a scratch address space:
 *
 *  - "foreign-mm": the space is not loaded anywhere else (a single-threaded
 *    process, or a teardown on another CPU).  There is nothing for remote
 *    CPUs to invalidate;
 *  - "active-mm": the space is the one loaded on the idle CPUs (the kernel
 *    pml4), so they really do hold user-range entries and must be told.
 *
 * Each case reports cycles/op and what the /proc/tlb_stats accounting saw:
 * IPIs per op and acknowledgements per op.  Gated on the kernel command line
 * like fb_bench/serial_bench; runs once after the APs are online and frees
 * the scratch address space when it is done.
 */
#define TLB_BENCH_ITERS 4000
#define TLB_BENCH_PASSES 3

static void tlb_bench_case(const char *name, uint64_t addr, uint64_t pml4,
                           uint32_t iters) {
    uint64_t best = 0;
    tlb_shootdown_reset_stats();

    for (uint32_t pass = 0; pass < TLB_BENCH_PASSES; pass++) {
        uint64_t t0 = rdtsc_fence();
        for (uint32_t i = 0; i < iters; i++)
            tlb_shootdown_page_for(addr, pml4);
        uint64_t cycles = rdtsc_fence() - t0;
        if (best == 0 || cycles < best)
            best = cycles;
    }

    tlb_shootdown_stats_t st;
    tlb_shootdown_get_stats(&st);

    uint64_t total = (uint64_t)iters * TLB_BENCH_PASSES;
    uint64_t ns = tsc_cycles_to_ns(best);
    klog_puts("[TLB-BENCH] ");
    klog_puts(name);
    klog_puts(": ");
    klog_uint64(best / iters);
    klog_puts(" cycles/op");
    if (ns) {
        klog_puts(", ");
        klog_uint64(ns / iters);
        klog_puts(" ns/op");
    }
    klog_puts(", IPIs/op x100=");
    klog_uint64(st.ipis_sent * 100 / total);
    klog_puts(", remote-acks/op x100=");
    klog_uint64(st.shootdowns * 100 / total);
    klog_puts("\n");
}

void tlb_bench_maybe_run(void) {
    extern const char *kernel_boot_cmdline;
    if (!kernel_boot_cmdline || !strstr(kernel_boot_cmdline, "tlb_bench"))
        return;
    if (cpu_get_count() < 2) {
        klog_puts("[TLB-BENCH] skipped: needs at least 2 CPUs\n");
        return;
    }

    uint64_t *scratch = vmm_create_pml4();
    void *frame = pmm_alloc();
    const uint64_t user_va = 0x400000;
    bool mapped = scratch && frame &&
                  vmm_map_page(scratch, user_va, (uint64_t)frame,
                               PAGE_FLAG_PRESENT | PAGE_FLAG_USER | PAGE_FLAG_RW);
    if (!mapped) {
        klog_puts("[TLB-BENCH] skipped: no scratch address space\n");
        if (frame && scratch)
            pmm_free_page(frame);
        if (scratch)
            vmm_free_user_pages((uint64_t)scratch);
        return;
    }

    klog_puts("\n[TLB-BENCH] single-page shootdown, ");
    klog_uint64(cpu_get_count());
    klog_puts(" CPUs, best of ");
    klog_uint64(TLB_BENCH_PASSES);
    klog_puts(" x ");
    klog_uint64(TLB_BENCH_ITERS);
    klog_puts(" iterations\n");

    tlb_bench_case("foreign-mm", user_va, (uint64_t)scratch, TLB_BENCH_ITERS);

    uint64_t active;
    __asm__ volatile("mov %%cr3, %0" : "=r"(active));
    tlb_bench_case("active-mm ", user_va, active & CR3_ADDR_MASK,
                   TLB_BENCH_ITERS);

    vmm_free_user_pages((uint64_t)scratch);
    klog_puts("[TLB-BENCH] done\n");
}
