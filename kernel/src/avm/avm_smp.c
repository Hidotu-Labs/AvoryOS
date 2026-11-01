#include "avm_smp.h"

#include "avm_irq.h"
#include "avm_lapic.h"

#include "../apic/lapic.h"
#include "../smp/cpu.h"
#include "../apic/lapic_timer.h"
#include "../lib/string.h"

/* ICR low word fields. */
#define ICR_VECTOR(low) ((low) & 0xFF)
#define ICR_MODE(low) (((low) >> 8) & 7)
#define ICR_LOGICAL(low) (((low) >> 11) & 1)
#define ICR_LEVEL(low) (((low) >> 14) & 1)
#define ICR_SHORTHAND(low) (((low) >> 18) & 3)

#define ICR_MODE_FIXED 0
#define ICR_MODE_INIT 5
#define ICR_MODE_SIPI 6

#define ICR_SH_NONE 0
#define ICR_SH_SELF 1
#define ICR_SH_ALL 2
#define ICR_SH_ALL_BUT_SELF 3

static uint32_t asid_counter;
static uint32_t asid_count;  /* ASIDs the CPU supports, from CPUID 8000_000Ah */

void avm_smp_init_vcpu(struct avm_vcpu *vcpu) {
  vcpu->mp_state = vcpu->id == 0 ? AVM_MP_RUNNABLE : AVM_MP_UNINIT;
}

bool avm_smp_wait_runnable(struct avm_vcpu *vcpu,
                           bool (*interrupted)(const struct avm_vcpu *vcpu)) {
  for (;;) {
    avm_irq_lock(vcpu->vm);
    bool ready = vcpu->mp_state == AVM_MP_RUNNABLE;
    avm_irq_unlock(vcpu->vm);
    if (ready)
      return true;
    /* QEMU stops a waiting vCPU with a signal, and it waits for the vCPU to
     * return.  A signal must end the wait, or the VMM hangs. */
    if (interrupted(vcpu))
      return false;
    lapic_timer_sleep(1);
  }
}

/* The register state a processor has after SIPI: real mode, with CS set from
 * the vector (CS base = vector * 4 KB, IP = 0).  Other segments start flat at
 * base 0 with 64 KB limits. */
static void sipi_reset(struct avm_vcpu *t, uint8_t vector) {
  memset(&t->regs, 0, sizeof(t->regs));
  t->regs.rflags = 2;
  t->regs.rip = 0;

  memset(&t->sregs, 0, sizeof(t->sregs));
  t->sregs.cs = (struct kvm_segment){
      .base = (uint64_t)vector << 12,
      .limit = 0xFFFF,
      .selector = (uint16_t)(vector << 8),
      .type = 0xB,
      .present = 1,
      .s = 1,
  };
  t->sregs.ds = (struct kvm_segment){ .limit = 0xFFFF, .type = 3, .present = 1, .s = 1 };
  t->sregs.es = t->sregs.ss = t->sregs.fs = t->sregs.gs = t->sregs.ds;
  t->sregs.tr = (struct kvm_segment){ .limit = 0xFFFF, .type = 0xB, .present = 1 };
  t->sregs.ldt = (struct kvm_segment){ .unusable = 1 };
  t->sregs.gdt.limit = 0xFFFF;
  t->sregs.idt.limit = 0xFFFF;
  t->sregs.cr0 = 0x10;
  t->state_dirty = true;
}

void avm_smp_write_icr(struct avm_vcpu *src, uint32_t dest, uint32_t low) {
  struct avm_vm *vm = src->vm;
  uint8_t vector = (uint8_t)ICR_VECTOR(low);
  uint32_t shorthand = ICR_SHORTHAND(low);

  /* Logical destinations are not modelled.  Shorthands ignore the field. */
  if (shorthand == ICR_SH_NONE && ICR_LOGICAL(low))
    return;

  for (uint32_t i = 0; i < AVM_MAX_VCPUS; i++) {
    struct avm_vcpu *t = vm->vcpus[i];
    if (!t)
      continue;

    bool hit;
    switch (shorthand) {
    case ICR_SH_SELF: hit = t == src; break;
    case ICR_SH_ALL: hit = true; break;
    case ICR_SH_ALL_BUT_SELF: hit = t != src; break;
    default: hit = dest == t->id; break;
    }
    if (!hit)
      continue;

    switch (ICR_MODE(low)) {
    case ICR_MODE_FIXED:
      avm_lapic_raise(t, vector);
      if (t != src)
        avm_smp_kick(t);
      break;
    case ICR_MODE_INIT:
      /* INIT asserts a reset: the APIC returns to its reset state, and the
       * processor waits for SIPI.  The level-deassert form is ignored. */
      if (!ICR_LEVEL(low))
        break;
      avm_lapic_init(t);
      t->mp_state = AVM_MP_INIT_DONE;
      avm_smp_kick(t);
      break;
    case ICR_MODE_SIPI:
      if (t->mp_state == AVM_MP_INIT_DONE) {
        sipi_reset(t, vector);
        t->mp_state = AVM_MP_RUNNABLE;
      }
      break;
    default:
      break;
    }
  }
}

/* Kicks are NMIs: a guest with IF clear would not take a maskable interrupt
 * while it runs, but NMIs do not depend on IF.  Each kick sent to a CPU adds
 * one here, and each kick NMI the host takes removes one.  The count matters:
 * several kicks can be in flight at once (memslot and routing changes kick every
 * vCPU), and a single flag would leave the second NMI looking stray.  An NMI
 * with no count still left is fatal. */
static uint32_t kick_expected[MAX_CPUS];

void avm_smp_kick(struct avm_vcpu *target) {
  if (!__atomic_load_n(&target->in_guest, __ATOMIC_ACQUIRE))
    return;
  /* The count and the NMI must name the same CPU.  Both come from one read of
   * host_cpu.  If the vCPU moves after that read, its old CPU still gets the NMI
   * and the count, so no CPU is left with an NMI it did not expect. */
  uint32_t cpu = target->host_cpu;
  __atomic_add_fetch(&kick_expected[cpu], 1, __ATOMIC_RELEASE);
  lapic_send_nmi(cpu_get_info(cpu)->apic_id);
}

bool avm_nmi_kick_consume(void) {
  uint32_t cpu = cpu_get_current()->cpu_id;
  if (cpu >= MAX_CPUS)
    return false;
  uint32_t n = __atomic_load_n(&kick_expected[cpu], __ATOMIC_ACQUIRE);
  while (n != 0) {
    if (__atomic_compare_exchange_n(&kick_expected[cpu], &n, n - 1, false,
                                    __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
      return true;
  }
  return false;
}

void avm_smp_kick_all(struct avm_vm *vm) {
  for (uint32_t i = 0; i < AVM_MAX_VCPUS; i++)
    if (vm->vcpus[i])
      avm_smp_kick(vm->vcpus[i]);
}

uint32_t avm_smp_alloc_asid(void) {
  /* One ASID per VM, cycling through the ASIDs the CPU supports.  The TLB is
   * flushed on every entry, so reusing an ASID cannot leave stale entries. */
  uint32_t n = __atomic_fetch_add(&asid_counter, 1, __ATOMIC_RELAXED);
  if (asid_count <= 1)
    return 1;
  return 1 + n % (asid_count - 1);
}

void avm_smp_set_asid_count(uint32_t count) { asid_count = count; }
