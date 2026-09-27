#include "smp/cpu.h"
#include "hal/hal.h"
#include "acpi/acpi.h"
#include "apic/lapic.h"
#include "apic/lapic_timer.h"
#include "console/console.h"
#include "console/klog.h"
#include "cpu/features.h"
#include "cpu/gdt.h"
#include "cpu/idt.h"
#include "drivers/timer/pit.h"
#include "lib/string.h"
#include "mm/pmm.h"
#include "mm/vmm.h"
#include <stddef.h>

// External Trampoline Symbols
extern uint8_t trampoline_start[];
extern uint8_t trampoline_end[];
extern uint8_t trampoline_data_cr3[];
extern uint8_t trampoline_data_rip[];
extern uint8_t trampoline_data_stack[];

// Storage
static struct cpu_info cpus[MAX_CPUS];
static uint32_t cpu_count = 0;

// Helpers

static void print_uint32(uint32_t num) {
  if (num == 0) {
    console_putchar('0');
    return;
  }
  char buf[10];
  int i = 0;
  while (num > 0) {
    buf[i++] = '0' + (num % 10);
    num /= 10;
  }
  while (i > 0) {
    console_putchar(buf[--i]);
  }
}

static void print_hex32(uint32_t num) {
  const char *hex = "0123456789ABCDEF";
  for (int i = 28; i >= 0; i -= 4) {
    console_putchar(hex[(num >> i) & 0xF]);
  }
}

// MSR helpers for GS base

// IA32_GS_BASE = 0xC0000101  (the actual GS.base used by the CPU)
// IA32_KERNEL_GS_BASE = 0xC0000102  (swapped in/out by SWAPGS)
#define MSR_GS_BASE 0xC0000101
#define MSR_KERNEL_GS_BASE 0xC0000102

static inline void wrmsr(uint32_t msr, uint64_t value) {
  uint32_t lo = (uint32_t)(value & 0xFFFFFFFF);
  uint32_t hi = (uint32_t)(value >> 32);
  __asm__ volatile("wrmsr" : : "c"(msr), "a"(lo), "d"(hi));
}

static inline uint64_t rdmsr(uint32_t msr) {
  uint32_t lo, hi;
  __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
  return ((uint64_t)hi << 32) | lo;
}

// Set GS base for the current CPU
static void cpu_set_gs_base(struct cpu_info *info) {
  wrmsr(MSR_GS_BASE, (uint64_t)info);
  wrmsr(MSR_KERNEL_GS_BASE, (uint64_t)info);
}

// Allocate a kernel stack (returns HHDM virtual address of top)
static uint64_t alloc_cpu_stack(void) {
  // We need CPU_STACK_SIZE bytes = multiple pages
  size_t pages = CPU_STACK_SIZE / PAGE_SIZE;
  void *phys = pmm_alloc_blocks(pages);
  if (!phys)
    return 0;

  // Convert to HHDM virtual address and return the TOP (stacks grow down)
  uint64_t virt_base = (uint64_t)phys + pmm_get_hhdm_offset();
  return virt_base + CPU_STACK_SIZE;
}

// Public API

struct cpu_info *cpu_get_current(void) {
  return (struct cpu_info *)rdmsr(MSR_GS_BASE);
}

uint32_t cpu_get_count(void) { return cpu_count; }

struct cpu_info *cpu_get_info(uint32_t cpu_id) {
  if (cpu_id >= cpu_count)
    return NULL;
  return &cpus[cpu_id];
}

struct cpu_info *cpu_get_bsp(void) { return &cpus[0]; }

static inline uint32_t get_initial_apic_id(void) {
  uint32_t ebx;
  __asm__ volatile("cpuid" : "=b"(ebx) : "a"(1) : "ecx", "edx");
  return ebx >> 24;
}

// AP Entry Point

static volatile struct cpu_info *starting_cpu = NULL;

void ap_main(void) {
  klog_puts("AP IN MAIN!\n");

  // We are now in 64-bit Long Mode!

  // 0. Load the proper full kernel GDT (replaces the temporary trampoline GDT)
  // This must be done FIRST because gdt_flush zeroes data segments like GS,
  // and we depend on the 64-bit code segment for subsequent interrupt handling.
  // Each AP also loads its own TSS so TSS.RSP0 is private to this core.
  struct cpu_info *bootstrap = (struct cpu_info *)starting_cpu;
  gdt_load_ap(bootstrap ? bootstrap->cpu_id : 0);
  cpu_features_init();

  // 1. Setup GS base using the pointer passed by the BSP
  cpu_set_gs_base((struct cpu_info *)starting_cpu);
  struct cpu_info *current = cpu_get_current();

  // 1.5 Switch to the dedicated kernel stack and page tables.
  // This is CRITICAL: APs must be off all bootloader memory (including tables)

  __asm__ volatile("mov %0, %%cr3" ::"r"(current->kernel_cr3) : "memory");
  cpu_switch_stack(current->stack_top);

  // 2. Initialize the LAPIC for this core (needs the HHDM base mapping)
  lapic_init((uint64_t)acpi_get_lapic_base());

  // 3. Load the IDT for this core (reuse the BSP's already-built table)
  idt_load();

  // 4. initialize the LAPIC timer for this core so it can independently preempt
  lapic_timer_init_ap();

  // 5. Enable interrupts locally on this core
  hal_irq_enable();

  // Mark as online to unblock the BSP's boot loop
  current->status = CPU_STATUS_ONLINE;

  klog_puts("     AP Woke up! CPU ");
  klog_uint64(current->cpu_id);
  klog_puts(" (APIC ID ");
  klog_hex32(current->apic_id);
  klog_puts(") ONLINE.\n");

  // Endless loop, waiting for IPIs or scheduler interrupts.
  //
  // The LAPIC is one-shot and only armed when this CPU has a deadline, so an
  // idle AP used to park in hlt forever and take exactly one tick ever.  That
  // made the BSP the only core running the hang detector (and the only core
  // polling the serial trigger): if the BSP stalled, the machine went silent
  // with no report at all.  Re-arming a slow deadline keeps every core taking
  // one tick a second, which is enough for all of them to notice a hang and
  // enough for any of them to read the serial console.
  while (1) {
    /* rearm_if_earlier(), not arm_at(): a sleeping thread's deadline may have
     * been armed by sched_arm_next_deadline() on the way into this idle loop,
     * and a plain rearm here would push that wakeup out to a full second. */
    lapic_timer_rearm_if_earlier(lapic_timer_get_ms() + 1000);
    hal_cpu_halt();
  }
}

// Initialization

void cpu_init(void) {
  console_puts(KLOG_CLR_GREEN "[  OK  ]" KLOG_CLR_RESET
                              " Initializing per-CPU data structures...\n");

  // Step 1: Query ACPI for all CPU APIC IDs
  cpu_count = acpi_get_cpu_count();
  if (cpu_count == 0) {
    console_puts(KLOG_CLR_RED
                 "[ FAIL ]" KLOG_CLR_RESET
                 " No CPUs found in MADT, assuming 1 (BSP only).\n");
    cpu_count = 1;
  }
  if (cpu_count > MAX_CPUS) {
    console_puts(KLOG_CLR_RED "[ FAIL ]" KLOG_CLR_RESET
                              " CPU count exceeds MAX_CPUS, clamping.\n");
    cpu_count = MAX_CPUS;
  }

  // Copy APIC IDs from the ACPI module into our per-CPU array
  const uint8_t *apic_ids = acpi_get_cpu_apic_ids();
  uint32_t bsp_apic_id = get_initial_apic_id();

  // Step 2: Populate cpu_info for each discovered CPU
  // Place BSP at index 0, APs at 1..N
  uint32_t bsp_index = 0;
  uint32_t ap_index = 1;

  for (uint32_t i = 0; i < cpu_count; i++) {
    uint8_t id = apic_ids[i];

    if (id == bsp_apic_id) {
      cpus[0].cpu_id = 0;
      cpus[0].apic_id = id;
      cpus[0].status = CPU_STATUS_BSP;
      cpus[0].self = &cpus[0];
      (void)bsp_index;
    } else {
      if (ap_index < cpu_count) {
        cpus[ap_index].cpu_id = ap_index;
        cpus[ap_index].apic_id = id;
        cpus[ap_index].status = CPU_STATUS_OFFLINE;
        cpus[ap_index].self = &cpus[ap_index];
        ap_index++;
      }
    }
  }

  // Step 3: Allocate kernel stacks
  for (uint32_t i = 0; i < cpu_count; i++) {
    cpus[i].stack_top = alloc_cpu_stack();
    if (cpus[i].stack_top == 0) {
      console_puts(KLOG_CLR_RED "[ FAIL ]" KLOG_CLR_RESET
                                " Failed to allocate stack for CPU ");
      print_uint32(i);
      console_puts("!\n");
    }
  }

  // Step 4: Read the kernel CR3 for the BSP
  uint64_t cr3;
  __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
  for (uint32_t i = 0; i < cpu_count; i++) {
    cpus[i].kernel_cr3 = cr3;
  }

  // Step 5: Set GS base for the BSP
  // We do this EARLY in cpu_init so that any early interrupts or code that
  // depends on cpu_get_current() (e.g. loggers or the early scheduler) works.
  cpu_set_gs_base(&cpus[0]);

  // Report
  console_puts(KLOG_CLR_GREEN "[  OK  ]" KLOG_CLR_RESET
                              " Per-CPU structures initialized.\n");
  console_puts("     Total CPUs: ");
  print_uint32(cpu_count);
  console_puts("\n");

  for (uint32_t i = 0; i < cpu_count; i++) {
    console_puts("     CPU ");
    print_uint32(cpus[i].cpu_id);
    console_puts(": APIC ID=0x");
    print_hex32(cpus[i].apic_id);
    console_puts(", Stack=0x");
    print_hex32((uint32_t)(cpus[i].stack_top >> 32));
    print_hex32((uint32_t)(cpus[i].stack_top & 0xFFFFFFFF));
    if (cpus[i].status == CPU_STATUS_BSP) {
      console_puts(" [BSP]");
    } else {
      console_puts(" [OFFLINE]");
    }
    console_puts("\n");
  }

  // Step 6: Verify GS base works
  struct cpu_info *current = cpu_get_current();
  if (current && current == &cpus[0] && current->status == CPU_STATUS_BSP) {
    console_puts(KLOG_CLR_GREEN "[  OK  ]" KLOG_CLR_RESET
                                " GS base self-pointer verified for BSP.\n");
  } else {
    console_puts(KLOG_CLR_RED "[ FAIL ]" KLOG_CLR_RESET
                              " GS base verification " KLOG_CLR_RED
                              "FAILED!" KLOG_CLR_RESET "\n");
  }
}

void cpu_init_aps(void) {
  // Step 7: Wake up the Application Processors
  if (cpu_count > 1) {
    console_puts(KLOG_CLR_GREEN "[  OK  ]" KLOG_CLR_RESET
                                " Waking up Application Processors...\n");

    uint64_t cr3;
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));

    uint64_t tramp_phys = 0x8000;
    uint64_t tramp_virt = tramp_phys + pmm_get_hhdm_offset();

    // Copy trampoline code to 0x8000
    memcpy((void *)tramp_virt, trampoline_start,
           trampoline_end - trampoline_start);

    // Identity-map the 0x8000 page in the active PML4 so the AP safely
    // transitions PAGING -> 64-bit Long Mode
    if (!vmm_map_page(vmm_get_active_pml4(), tramp_phys, tramp_phys,
                      PAGE_FLAG_RW | PAGE_FLAG_PRESENT)) {
      klog_puts(KLOG_CLR_RED "[ FAIL ]" KLOG_CLR_RESET
                             " Warning: Failed to map trampoline page\n");
    }

    // Find offsets to modify the trampoline variables natively
    uint64_t cr3_offset = trampoline_data_cr3 - trampoline_start;
    uint64_t rip_offset = trampoline_data_rip - trampoline_start;
    uint64_t stack_offset = trampoline_data_stack - trampoline_start;

    volatile uint64_t *ptr_cr3 = (volatile uint64_t *)(tramp_virt + cr3_offset);
    volatile uint64_t *ptr_rip = (volatile uint64_t *)(tramp_virt + rip_offset);
    volatile uint64_t *ptr_stack =
        (volatile uint64_t *)(tramp_virt + stack_offset);

    // We only write these once since they are globally identical for all APs
    *ptr_cr3 = cr3;
    *ptr_rip = (uint64_t)ap_main;

    for (uint32_t i = 1; i < cpu_count; i++) {
      if (cpus[i].status == CPU_STATUS_BSP)
        continue;

      // Give this AP its distinct stack
      *ptr_stack = cpus[i].stack_top;

      // Set the baton so `ap_main` knows who to map its GS-base to
      starting_cpu = &cpus[i];

      // 1. Send INIT IPI (Edge Triggered, Physical Destination)
      lapic_write(LAPIC_ICR_HIGH, cpus[i].apic_id << 24);
      lapic_write(LAPIC_ICR_LOW, LAPIC_ICR_INIT);

      // 2. Wait 10ms
      lapic_timer_sleep(10);

      // 3. Send STARTUP SIPI (Edge Triggered, Physical Destination)
      uint8_t vector = tramp_phys >> 12; // 0x08
      lapic_write(LAPIC_ICR_HIGH, cpus[i].apic_id << 24);
      lapic_write(LAPIC_ICR_LOW, vector | LAPIC_ICR_STARTUP);

      // Wait for AP to finish booting and signal ONLINE
      while (cpus[i].status != CPU_STATUS_ONLINE) {
        hal_cpu_relax();
      }
    }
  }
}
