#include "tlb_shootdown.h"
#include "hal/hal.h"
#include "../apic/lapic.h"
#include "../console/klog.h"
#include "../cpu/isr.h"
#include "../sched/sched.h"
#include "../smp/cpu.h"
#include "pcid.h"
#include "vmm.h"
#include "../lock/spinlock.h"
#include "../lock/lockdiag.h"
#include "../cpu/features.h"
#include "../cpu/kpf_dump.h"
#include "../drivers/serial.h"
#include "../lib/tsc.h"
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* -------------------------------------------------------------------------
 * Per-CPU TLB shootdown state
 * ------------------------------------------------------------------------- */

/* Global lock for shootdown serialization (without disabling interrupts to avoid IPI deadlock). */
static rawspinlock_t shootdown_lock = RAWSPINLOCK_INIT;

/* Per-CPU pending virtual address (written by initiator, read by target). */
#define MAX_CPUS 64
static volatile uint64_t cpu_shootdown_addr[MAX_CPUS];
static volatile uint8_t  cpu_shootdown_ack[MAX_CPUS];
/* PCID the stale translations are tagged with, or PCID_KERNEL when the
 * initiator could not tell us (it was modifying an address space other than
 * the one loaded on its own CPU). */
static volatile uint16_t cpu_shootdown_pcid[MAX_CPUS];

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
                                uint64_t addr, uint64_t source_cr3) {
    (void)addr;
    (void)source_cr3;
    if (!cpu || cpu == self)
        return false;
    if (cpu->status != CPU_STATUS_ONLINE && cpu->status != CPU_STATUS_BSP)
        return false;
    /* Broadcast shootdown to all online remote CPUs. With lazy CR3 and
     * PCID-enabled context switching (CR3_NOFLUSH), any remote CPU may hold
     * stale cached translations for user address spaces even when idle or
     * currently switched away. */
    return true;
}

void tlb_shootdown_handle_ipi(void) {
    struct cpu_info *self = cpu_get_current();
    if (!self)
        return;

    uint32_t id = self->cpu_id;
    if (id >= MAX_CPUS)
        return;

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

    /* Ack: store 0 to signal the initiator we are done. */
    __atomic_store_n(&cpu_shootdown_ack[id], 0, __ATOMIC_RELEASE);
}

static void tlb_shootdown_isr(struct registers *regs) {
    (void)regs;
    tlb_shootdown_handle_ipi();
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
 * safe, losing an append never is.  When the list fills up it collapses into a
 * single full invalidation, which is conservative and bounded, so a large mmap
 * or unmap costs one broadcast instead of one per page. */
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
  if (n >= TLB_DEFER_SLOTS) {
    /* Too many distinct addresses: one broadcast is cheaper than tracking them
     * and strictly more conservative. */
    __atomic_store_n(&tlb_pending_all[slot], 1, __ATOMIC_RELAXED);
    __atomic_store_n(&tlb_pending_count[slot], 0, __ATOMIC_RELAXED);
    hal_irq_restore(flags);
    return;
  }
  tlb_pending[slot][n].addr = addr;
  tlb_pending[slot][n].pml4 = pml4;
  __atomic_store_n(&tlb_pending_count[slot], n + 1, __ATOMIC_RELAXED);
  hal_irq_restore(flags);
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

void tlb_flush_deferred_drain(void) {
  int slot = tlb_self_slot();
  if (slot < 0)
    return;

  hal_irq_state_t flags = hal_irq_save();
  uint8_t all = __atomic_load_n(&tlb_pending_all[slot], __ATOMIC_RELAXED);
  uint32_t n = __atomic_load_n(&tlb_pending_count[slot], __ATOMIC_RELAXED);
  if (!all && n == 0) {
    hal_irq_restore(flags);
    return;
  }
  __atomic_store_n(&tlb_pending_all[slot], 0, __ATOMIC_RELAXED);
  __atomic_store_n(&tlb_pending_count[slot], 0, __ATOMIC_RELAXED);
  hal_irq_restore(flags);

  /* Only the last request for an address matters, and a duplicate flush is
   * harmless, so the list is replayed as recorded. */
  if (all) {
    tlb_shootdown_all();
    return;
  }
  for (uint32_t i = 0; i < n; i++)
    tlb_shootdown_page_for(tlb_pending[slot][i].addr,
                           tlb_pending[slot][i].pml4);
}

static void do_shootdown(uint64_t addr, uint16_t pcid, uint64_t caller_ip) {
    uint32_t cpu_count = cpu_get_count();
    struct cpu_info *self = cpu_get_current();
    uint64_t source_cr3;
    __asm__ volatile("mov %%cr3, %0" : "=r"(source_cr3));

    uint32_t targets = 0;
    for (uint32_t i = 0; i < cpu_count && i < MAX_CPUS; i++) {
        struct cpu_info *c = cpu_get_info(i);
        if (cpu_needs_shootdown(c, self, addr, source_cr3))
            targets++;
    }

    if (targets == 0) {
        local_flush(addr, pcid);
        return;
    }

    rawspinlock_acquire(&shootdown_lock);

    LOCKDIAG_STAT(shootdowns, 1);
    tlb_stat_add(&tlb_stats.shootdowns, 1);
    tlb_stat_add(&tlb_stats.ipis_sent, targets);
    if (addr == TLB_SHOOTDOWN_ALL)
        tlb_stat_add(&tlb_stats.broadcast_all, 1);

    /* Publish address to all target CPUs. */
    for (uint32_t i = 0; i < cpu_count && i < MAX_CPUS; i++) {
        struct cpu_info *c = cpu_get_info(i);
        if (!cpu_needs_shootdown(c, self, addr, source_cr3))
            continue;
        __atomic_store_n(&cpu_shootdown_pcid[c->cpu_id], pcid, __ATOMIC_RELEASE);
        __atomic_store_n(&cpu_shootdown_addr[c->cpu_id], addr, __ATOMIC_RELEASE);
        __atomic_store_n(&cpu_shootdown_ack[c->cpu_id], 1, __ATOMIC_RELEASE);
    }

    /* Local flush first - overlaps with IPI delivery latency. */
    local_flush(addr, pcid);

    /* Send all IPIs in one burst. */
    for (uint32_t i = 0; i < cpu_count && i < MAX_CPUS; i++) {
        struct cpu_info *c = cpu_get_info(i);
        if (!cpu_needs_shootdown(c, self, addr, source_cr3))
            continue;
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
    for (uint32_t i = 0; i < cpu_count && i < MAX_CPUS; i++) {
        struct cpu_info *c = cpu_get_info(i);
        if (!c || c == self) continue;
        if (c->status != CPU_STATUS_ONLINE && c->status != CPU_STATUS_BSP) continue;
        if (!cpu_needs_shootdown(c, self, addr, source_cr3)) continue;
        while (__atomic_load_n(&cpu_shootdown_ack[c->cpu_id], __ATOMIC_ACQUIRE) != 0) {
            hal_cpu_relax();
            if (deadline) {
                if (rdtsc() > deadline)
                    shootdown_stuck(self, c, addr);
            } else if (--fallback_spins == 0) {
                shootdown_stuck(self, c, addr);
            }
        }
    }

    uint64_t waited = rdtsc() - wait_start;
    if (mhz) {
        tlb_stat_add(&tlb_stats.ack_wait_ns, waited / mhz);
        tlb_stat_max(&tlb_stats.max_ack_wait_ns, waited / mhz);

        /* Waited time in real milliseconds.  Note mhz is TSC ticks per
         * MICROSECOND, so a millisecond is mhz*1000 ticks: the deadline above
         * uses mhz alone, which makes SHOOTDOWN_TIMEOUT_MS a 10 ms timeout, not
         * 10 s.  Measuring here makes that visible either way. */
        uint64_t waited_ms = waited / (mhz * 1000ULL);
        LOCKDIAG_STAT_MAX(shootdown_max_ack_ns,
                          tsc_cycles_to_ns(waited));
        if (waited_ms > LOCKDIAG_SLOW_ACK_MS) {
            LOCKDIAG_STAT(shootdown_slow_acks, 1);
            sd_note("\n[TLB] SLOW ack wait=");
            sd_dec(waited_ms);
            sd_note("ms addr=");
            sd_hex(addr);
            sd_note(" targets=");
            sd_dec(targets);
            sd_note(" initiator_cpu=");
            sd_dec(self ? self->cpu_id : 0xFFFFFFFFFFFFFFFFULL);
            sd_note("\n");
        }
    }

    rawspinlock_release(&shootdown_lock);
}

void tlb_shootdown_page(uint64_t addr) {
    /*
     * Assumes the affected address space is the one loaded on this CPU, which
     * is true for the fault path and for every syscall touching its own mm.
     * Anything manipulating another address space must use
     * tlb_shootdown_page_for(), or remote CPUs will be told to flush the wrong
     * PCID.
     */
    do_shootdown(addr, local_pcid(), (uint64_t)__builtin_return_address(0));
}

void tlb_shootdown_page_for(uint64_t addr, uint64_t pml4) {
    uint16_t pcid = PCID_KERNEL;
    if (cpu_has_pcid()) {
        uint64_t cr3;
        __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
        if ((cr3 & CR3_ADDR_MASK) == (pml4 & CR3_ADDR_MASK))
            pcid = (uint16_t)(cr3 & CR3_PCID_MASK);
        /* Otherwise the caller is tearing down an address space that is not
         * loaded here, and we genuinely do not know which PCID owns the stale
         * entries: PCID_KERNEL makes every target behave conservatively. */
    }
    do_shootdown(addr, pcid, (uint64_t)__builtin_return_address(0));
}

void tlb_shootdown_all(void) {
    do_shootdown(TLB_SHOOTDOWN_ALL, PCID_KERNEL,
                 (uint64_t)__builtin_return_address(0));
}
