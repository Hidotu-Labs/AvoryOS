#include "isr.h"
#include "isr_internal.h"
#include "../mm/pmm.h"
#include "../mm/vmm.h"
#include "../smp/cpu.h"
#include "apic/lapic.h"
#include "pic.h"

static isr_t interrupt_handlers[256] = {0};
static bool apic_mode = false;
static bool allocated_device_vectors[256] = {0};
static uint64_t vector_interrupt_counts[MAX_CPUS][256];

uint64_t isr_get_vector_count(uint32_t cpu_id, uint32_t vector) {
  if (cpu_id >= MAX_CPUS || vector >= 256)
    return 0;
  return __atomic_load_n(&vector_interrupt_counts[cpu_id][vector],
                         __ATOMIC_RELAXED);
}

void isr_record_vector_delivery(uint32_t cpu_id, uint32_t vector) {
  if (cpu_id < MAX_CPUS && vector < 256)
    __atomic_fetch_add(&vector_interrupt_counts[cpu_id][vector], 1,
                       __ATOMIC_RELAXED);
}

void isr_set_apic_mode(bool enabled) { apic_mode = enabled; }

void register_interrupt_handler(uint8_t n, isr_t handler) {
  interrupt_handlers[n] = handler;
}

int interrupt_vector_alloc(isr_t handler) {
  if (!handler) return -1;
  /* 0x60..0xDF avoids exceptions, legacy IRQs, timer/IPIs and spurious. */
  for (uint16_t vector = 0x60; vector <= 0xDF; vector++) {
    if (vector == 0x80) continue; /* Linux i386 software syscall gate */
    if (!__atomic_test_and_set(&allocated_device_vectors[vector],
                               __ATOMIC_ACQ_REL)) {
      interrupt_handlers[vector] = handler;
      return (int)vector;
    }
  }
  return -1;
}

void interrupt_vector_free(uint8_t vector) {
  if (vector < 0x60 || vector > 0xDF) return;
  interrupt_handlers[vector] = NULL;
  __atomic_clear(&allocated_device_vectors[vector], __ATOMIC_RELEASE);
}

static void send_eoi(struct registers *regs) {
  if (regs->int_no == 255)
    return;
  if (regs->int_no < 32)
    return;
  /* lapic_timer_handler (vector 48) acknowledges its LAPIC EOI before calling
   * sched_tick(). A thread preempted inside sched_tick() may migrate and
   * resume on another CPU, where a second EOI here would acknowledge the wrong
   * CPU's in-service vector. */
  if (regs->int_no == 48)
    return;
  if (apic_mode) {
    /* x2APIC EOI is an MSR write and does not need the LAPIC HHDM mapping.
     * In xAPIC mode, however, a process PML4 can lack the MMIO slot even
     * though the permanent kernel PML4 maps it. Switch only around the EOI
     * store so the interrupt is acknowledged without faulting, then restore
     * the interrupted address space before returning to the ISR epilogue. */
    uint64_t saved_cr3 = 0;
    bool switched_cr3 = false;
    uint64_t lapic_va = lapic_get_va();
    if (!lapic_is_x2apic() && lapic_va) {
      __asm__ volatile("mov %%cr3, %0" : "=r"(saved_cr3));
      uint64_t active_cr3 = saved_cr3 & PAGE_MASK;
      if (!vmm_debug_walk(active_cr3, lapic_va, NULL)) {
        static volatile unsigned eoi_reported;
        if (!__atomic_exchange_n(&eoi_reported, 1, __ATOMIC_ACQ_REL)) {
          vmm_debug_dump_walk("eoi-missing active", active_cr3, lapic_va);
          vmm_debug_dump_walk("eoi-missing kernel",
                              (uint64_t)(uintptr_t)vmm_get_kernel_pml4(),
                              lapic_va);
        }
        uint64_t kernel_cr3 =
            (uint64_t)(uintptr_t)vmm_get_kernel_pml4() & PAGE_MASK;
        if (kernel_cr3 && kernel_cr3 != active_cr3) {
          __asm__ volatile("mov %0, %%cr3" : : "r"(kernel_cr3) : "memory");
          switched_cr3 = true;
        }
      }
    }
    lapic_send_eoi();
    if (switched_cr3)
      __asm__ volatile("mov %0, %%cr3" : : "r"(saved_cr3) : "memory");
  } else if (regs->int_no <= 47) {
    pic_send_eoi(regs->int_no - 32);
  }
}

bool isr_interrupt_dispatch(struct registers *regs) {
  if (interrupt_handlers[regs->int_no] != 0) {
    isr_t handler = interrupt_handlers[regs->int_no];
    handler(regs);
    send_eoi(regs);
    return true;
  }

  if (regs->int_no >= 32) {
    send_eoi(regs);
    return true;
  }

  return false;
}