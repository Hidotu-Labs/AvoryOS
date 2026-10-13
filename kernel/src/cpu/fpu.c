/* Lazy restore with eager save of live owners. Never retain another task's
 * registers behind CR0.TS: scrub them before dispatch (LazyFP mitigation).
 * An off-CPU task's buffer is always authoritative, including on migration. */
#include "fpu.h"
#include "features.h"
#include "../sched/sched.h"
#include "../smp/cpu.h"
#include "../hal/hal.h"
#include "../lib/string.h"
#include "../console/klog.h"

/* Deterministic architectural defaults; do not snapshot the creator's XMM
 * registers or execute FNINIT while creating a task. XSTATE_BV selects the
 * legacy defaults below; absent extended components restore to init state. */
static const uint8_t initial_fpu_state[4096] __attribute__((aligned(64))) = {
    [0] = 0x7f, [1] = 0x03,             /* x87 control word */
    [24] = 0x80, [25] = 0x1f,          /* MXCSR */
    [28] = 0xff, [29] = 0xff,          /* MXCSR_MASK */
    [512] = 3,                        /* XSTATE_BV: x87 + SSE */
};

static inline void cr0_clear_ts(void) {
    __asm__ volatile("clts" ::: "memory");
}

static inline void cr0_set_ts(void) {
    uint64_t cr0;
    __asm__ volatile("mov %%cr0, %0" : "=r"(cr0));
    __asm__ volatile("mov %0, %%cr0" :: "r"(cr0 | 8) : "memory");
}

static void save_state(void *area) {
    if (cpu_has_xsave_flag) {
        uint64_t mask = __atomic_load_n(&cpu_xsave_mask, __ATOMIC_ACQUIRE);
        __asm__ volatile("xsave64 (%0)" :: "r"(area), "a"((uint32_t)mask),
                         "d"((uint32_t)(mask >> 32)) : "memory");
    } else {
        __asm__ volatile("fxsave64 (%0)" :: "r"(area) : "memory");
    }
}

static void restore_state(const void *area) {
    if (cpu_has_xsave_flag) {
        uint64_t mask = __atomic_load_n(&cpu_xsave_mask, __ATOMIC_ACQUIRE);
        __asm__ volatile("xrstor64 (%0)" :: "r"(area), "a"((uint32_t)mask),
                         "d"((uint32_t)(mask >> 32)) : "memory");
    } else {
        __asm__ volatile("fxrstor64 (%0)" :: "r"(area) : "memory");
    }
}

void fpu_init_thread(struct thread *t) {
    memcpy(t->fpu_state, initial_fpu_state, sizeof(t->fpu_state));
}

/* Called locally after GS and IDT exist, before this CPU dispatches tasks. */
void fpu_init_cpu(void) {
    hal_irq_state_t flags = hal_irq_save();
    cr0_clear_ts();
    restore_state(initial_fpu_state);
    cpu_get_current()->fpu_owner = NULL;
    cr0_set_ts();
    hal_irq_restore(flags);
}

/* IRQs are disabled, current_thread still names the departing thread. */
void fpu_on_context_switch(void) {
    struct cpu_info *cpu = cpu_get_current();
    /* No owner implies TS is already set and hardware is clean. This is
     * the common integer-only path: no CR0 write and no state-area access. */
    if (!cpu->fpu_owner)
        return;
    cr0_clear_ts();
    save_state(cpu->fpu_owner->fpu_state);
    restore_state(initial_fpu_state);
    cpu->fpu_owner = NULL;
    cr0_set_ts();
}

void fpu_ensure_loaded(const struct thread *t) {
    hal_irq_state_t flags = hal_irq_save();
    struct cpu_info *cpu = cpu_get_current();
    cr0_clear_ts();
    if (cpu && t && cpu->fpu_owner != t) {
        restore_state(t->fpu_state);
        cpu->fpu_owner = (struct thread *)t;
    }
    hal_irq_restore(flags);
}

void fpu_nm_handler(struct registers *regs) {
    (void)regs;
    fpu_ensure_loaded(sched_get_current());
}

/* Snapshot a running parent's state without making an integer-only parent
 * an FPU owner. Child is not runnable yet. */
void fpu_copy_thread(struct thread *child, const struct thread *parent) {
    hal_irq_state_t flags = hal_irq_save();
    struct cpu_info *cpu = cpu_get_current();
    if (cpu->fpu_owner == parent) {
        cr0_clear_ts();
        save_state(cpu->fpu_owner->fpu_state);
    }
    memcpy(child->fpu_state, parent->fpu_state, sizeof(child->fpu_state));
    hal_irq_restore(flags);
}

void fpu_reset_current(void) {
    hal_irq_state_t flags = hal_irq_save();
    struct cpu_info *cpu = cpu_get_current();
    if (cpu->fpu_owner) {
        cr0_clear_ts();
        restore_state(initial_fpu_state);
        cpu->fpu_owner = NULL;
    }
    fpu_init_thread(cpu->current_thread);
    cr0_set_ts();
    hal_irq_restore(flags);
}

void fpu_forget_thread(struct thread *t) {
    /* Reaping requires the task to be off-CPU. The switch-out has already
     * dropped ownership; no remote CPU's live registers may be touched here. */
    (void)t;
}

void fpu_init(void) {
    cr0_clear_ts(); /* Early boot has no GS/current thread yet. */
    register_interrupt_handler(7, fpu_nm_handler);
    klog_debug_puts("[FPU] Lazy restore with scrubbed switch-out initialized\n");
}
