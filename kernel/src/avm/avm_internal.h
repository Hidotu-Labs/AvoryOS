#ifndef AVM_AVM_INTERNAL_H
#define AVM_AVM_INTERNAL_H

#include "avm_uapi.h"

#include "../lock/spinlock.h"

#include <stdbool.h>
#include <stdint.h>

#define AVM_MAX_SLOTS 32
#define AVM_MAX_VCPUS 8
#define AVM_MAX_CPUID 80 /* KVM_MAX_CPUID_ENTRIES */
#define AVM_MAX_IOEVENTFDS 16
/* Banks reported for machine checks.  Their MSRs are not emulated, and the
 * CPUID table hides MCE and MCA, so no guest uses them. */
#define AVM_MCE_BANKS 32
/* The machine-check capability: MCG_CTL_P and the bank count.  This is what
 * KVM_X86_GET_MCE_CAP_SUPPORTED reports.  KVM_X86_SETUP_MCE accepts MCG_CTL_P
 * and any bank count from one up to AVM_MCE_BANKS. */
#define MCG_CTL_P (1ull << 8)
#define AVM_MCG_CAP_SUPPORTED (MCG_CTL_P | AVM_MCE_BANKS)
#define AVM_MAX_ROUTES 1024 /* one per GSI at most, matching AVM_MAX_GSI */
#define AVM_MAX_IRQFDS 16
#define AVM_PAGE_SIZE 4096ull

struct avm_slot {
  uint32_t id;
  uint64_t gpa;
  uint64_t size;
  uint64_t hva;
  bool used;
  bool readonly;  /* KVM_MEM_READONLY: writes exit to the VMM */
  bool log_dirty; /* KVM_MEM_LOG_DIRTY_PAGES: KVM_GET_DIRTY_LOG is served */
};

struct avm_vm;

/* In-kernel local APIC, in x2APIC mode.  Timer deadlines are host
 * monotonic nanoseconds.  The guest timer counts 1 MHz divided by the divide
 * configuration, so one count is 1 us times the divisor. */
struct avm_lapic {
  uint64_t apic_base;
  uint32_t svr;
  uint32_t tpr;
  uint32_t esr;
  uint32_t lvt[6];  /* timer, thermal, perf, lint0, lint1, error */
  uint32_t timer_init;
  uint32_t timer_div;
  bool timer_armed;
  uint64_t timer_deadline_ns;
  uint64_t timer_period_ns;
  uint32_t irr[8];
  uint32_t isr[8];
  uint32_t tmr[8];
};

/* One 8259 PIC.  Two of them make the master/slave pair, with the slave on
 * master IRQ 2.  Priority is fixed: IRQ 0 is highest. */
struct avm_pic {
  uint8_t irr;
  uint8_t isr;
  uint8_t imr;
  uint8_t vector_base;
  uint8_t line;        /* input levels, to detect rising edges */
  uint8_t init_step;   /* 0 = running; 1..3 = waiting for ICW2..ICW4 */
  bool init_cascade;   /* ICW1: a slave is attached */
  bool init_icw4;      /* ICW1: ICW4 follows */
  bool read_isr;       /* OCW3: command reads return ISR instead of IRR */
};

/* IOAPIC with 24 pins.  Its registers are reached through an index register
 * and a data register in MMIO space. */
struct avm_ioapic {
  uint32_t index;
  uint64_t redir[24];  /* redirection entries; reset value masks every pin */
  uint32_t line;       /* input levels, to detect rising edges */
};

/* PIT channel 0.  The input clock is 1193182 Hz.  Only modes 0, 2 and 3 are
 * modelled; modes 1, 4 and 5 act as mode 0. */
struct avm_pit {
  bool created;
  uint8_t access;       /* 1 low byte, 2 high byte, 3 low then high */
  uint8_t mode;
  uint8_t write_phase;  /* access 3: 0 expects the low byte, 1 the high byte */
  uint8_t write_lo;
  uint8_t read_phase;
  uint16_t reload;      /* 0 means 65536 */
  bool armed;           /* counting and able to raise IRQ 0 */
  bool periodic;
  uint64_t start_ns;
  uint64_t next_irq_ns;
  uint64_t period_ns;
  bool latched;
  uint16_t latch;
};

/* Multiprocessor state of a vCPU, driven by INIT and SIPI. */
#define AVM_MP_UNINIT 0     /* waiting for INIT (application processors start here) */
#define AVM_MP_INIT_DONE 1  /* INIT received, waiting for SIPI */
#define AVM_MP_RUNNABLE 2   /* running, or ready to run */

/* Variable-range MTRR pairs (base and mask), as MSR_MTRRcap_VCNT in QEMU. */
#define AVM_MTRR_VCNT 8

/* MSRs that only the VMM sets and reads, through KVM_SET_MSRS and
 * KVM_GET_MSRS.  The guest cannot see them.  QEMU writes most of them on every
 * vCPU reset, so each must round-trip; if one fails, QEMU stops with an error.
 * The guest-visible MSRs live in the VMCB instead (svm_msr_get). */
struct avm_vmm_msrs {
  uint64_t tsc;
  uint64_t mtrr_deftype;
  uint64_t mtrr_fixed[11];        /* 0x250, 0x258, 0x259, 0x268..0x26F */
  uint64_t mtrr_var[2 * AVM_MTRR_VCNT];
  uint64_t mcg_status, mcg_ctl;
  /* The legacy AMD performance counters (K7 EVNTSEL 0xC0010000.., PERFCTR
   * 0xC0010004..).  QEMU writes them for every AMD vCPU; no counter runs. */
  uint64_t k7_evntsel[4], k7_perfctr[4];
  uint64_t mc[AVM_MCE_BANKS * 4]; /* 0x400.., four MSRs per bank */
};

struct avm_vcpu {
  struct avm_vm *vm;
  uint32_t id;
  struct kvm_regs regs;
  struct kvm_sregs sregs;
  struct avm_lapic lapic;

  /* Multiprocessor: INIT/SIPI state, and where the vCPU is running.  The host
   * CPU's APIC ID is recorded while the vCPU is in guest mode, so another CPU
   * can kick it out of the guest. */
  uint8_t mp_state;
  bool in_guest;
  /* Set only while the vCPU is inside VMRUN, where it can use the NPT.  A halted
   * vCPU sets in_guest but not this. */
  bool in_vmrun;
  uint32_t host_apic_id;
  uint32_t host_cpu;  /* the host CPU's logical index, for the kick flag */
  uint32_t icr_high;  /* xAPIC ICR high dword, written before the low dword */
  /* The host CPU of the last entry, to log when the vCPU moves. */
  uint32_t last_apic_id;
  bool entered;
  /* The guest's XCR0, as set by its XSETBV.  The host's XCR0 never changes. */
  uint64_t xcr0;
  /* Guest FPU/SIMD state, saved and restored around each VMRUN.  Zero size
   * means XSAVE is unavailable and the state is not switched. */
  uint64_t xsave_pa;

  /* KVM_SET_SIGNAL_MASK: the signals blocked while KVM_RUN runs. */
  bool has_sigmask;
  uint64_t sigmask;

  /* KVM_SET_GUEST_DEBUG: single-step, and whether interrupts are held back
   * while stepping. */
  bool singlestep;
  bool block_irq;
  /* An instruction completed in single-step mode, so the VMM is told before the
   * guest runs again. */
  bool step_exit;
  /* KVM_X86_SETUP_MCE: the machine-check capability the VMM chose. */
  uint64_t mcg_cap;
  /* The VMM's view of the MSRs in avm_vmm_msrs.  The guest does not see them. */
  struct avm_vmm_msrs vmm_msrs;

  /* CPUID table that guest CPUID reads come from.  It starts as the default
   * table and is replaced by KVM_SET_CPUID2.  The host's CPUID is not used. */
  uint32_t cpuid_nent;
  struct kvm_cpuid_entry2 cpuid[AVM_MAX_CPUID];

  /* Set by KVM_SET_REGS/SREGS; the next KVM_RUN writes them into the VMCB. */
  bool state_dirty;
  /* The last exit was an IN; the VMM's value goes into RAX on re-entry. */
  bool io_in_pending;
  uint8_t io_in_size;
  /* The last exit was an INS.  The VMM's data goes to guest memory on
   * re-entry: io_str_count elements of io_str_size bytes, at io_str_base plus
   * (io_str_start + i * io_str_step) & io_str_mask. */
  bool io_str_pending;
  uint8_t io_str_size;
  uint64_t io_str_count;
  uint64_t io_str_base;
  uint64_t io_str_start;
  uint64_t io_str_step;
  uint64_t io_str_mask;
  /* Event to inject on the next VMRUN (VMCB EVENTINJ format), or 0. */
  uint64_t pending_event;
  /* A memory access that went to the VMM as KVM_EXIT_MMIO.  The VMM's answer
   * (for a read) goes into the register when the vCPU re-enters. */
  bool mmio_pending;
  bool mmio_read;
  bool mmio_reg8;
  uint8_t mmio_size;
  uint8_t mmio_reg;
  uint8_t mmio_insn_len;
  /* The access is one element of a string instruction.  RIP does not move
   * until the last element, and the index registers move on each time. */
  bool mmio_str;
  bool mmio_str_rep;
  bool mmio_str_si;  /* advance RSI: LODS and MOVS */
  bool mmio_str_di;  /* advance RDI: STOS and MOVS */
  uint64_t mmio_str_step;
  uint64_t mmio_str_mask;

  uint64_t vmcb_pa;   /* 4 KB VMCB: control area and state save area */
  uint64_t iopm_pa;   /* 12 KB I/O permission map, physically contiguous */
  uint64_t msrpm_pa;  /* 8 KB MSR permission map, physically contiguous */
  uint64_t run_pa;    /* shared kvm_run page, mapped into the VMM by mmap */
  struct kvm_run *run;
};

/* One ioeventfd registration (see avm_ioeventfd.c).  The eventfd reference is
 * held while the entry is used. */
struct avm_ioeventfd {
  bool used;
  bool pio;
  bool datamatch;
  uint32_t len;
  uint64_t addr;
  uint64_t match;  /* the datamatch value, masked to len */
  void *efd;
};

/* One irqfd (see avm_irqroute.c).  The eventfd reference is held while used. */
struct avm_irqfd {
  bool used;
  uint32_t gsi;
  void *efd;
  struct avm_vm *vm;
};

/* Marks an instruction as completed, for single-step.  The kernel calls this
 * wherever it advances RIP past an instruction it handled, or the VMM answered. */
static inline void avm_step_done(struct avm_vcpu *vcpu) {
  if (vcpu->singlestep)
    vcpu->step_exit = true;
}

struct avm_vm {
  /* One reference per open fd (the VM fd and each vCPU fd). */
  uint32_t refs;
  spinlock_t lock;
  /* Set while a memslot is being pinned.  Pinning can fault and sleep, so it
   * runs with the lock dropped; this flag keeps other memslot ops out. */
  bool slot_busy;
  /* Host-physical address of the nested PML4 (NPT root). */
  uint64_t npt_root;
  /* Set by KVM_CREATE_IRQCHIP.  Without it, APIC MSRs raise #GP. */
  bool irqchip;
  /* Address space ID for this VM's guest TLB entries. */
  uint32_t asid;
  /* Guards the PIC and PIT state.  The VMM can raise IRQ lines from its own
   * thread while a vCPU runs.  Held only for short, non-sleeping sections. */
  spinlock_t irq_lock;
  struct avm_pic pic[2];  /* master, slave */
  struct avm_pit pit;
  struct avm_ioapic ioapic;
  struct avm_slot slots[AVM_MAX_SLOTS];
  struct avm_vcpu *vcpus[AVM_MAX_VCPUS];
  struct avm_ioeventfd ioeventfds[AVM_MAX_IOEVENTFDS];
  /* GSI routing (KVM_SET_GSI_ROUTING).  Guarded by irq_lock. */
  struct kvm_irq_routing_entry routes[AVM_MAX_ROUTES];
  uint32_t nroutes;
  struct avm_irqfd irqfds[AVM_MAX_IRQFDS];
};

/* avm_mem.c */
int avm_mem_vm_init(struct avm_vm *vm);
void avm_mem_vm_destroy(struct avm_vm *vm);
int avm_mem_map_slot(struct avm_vm *vm, const struct avm_slot *slot);
int avm_mem_unmap_slot(struct avm_vm *vm, const struct avm_slot *slot);

#endif /* AVM_AVM_INTERNAL_H */
