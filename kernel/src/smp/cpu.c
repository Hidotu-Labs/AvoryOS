#include "smp/cpu.h"
#include "hal/hal.h"
#include "acpi/acpi.h"
#include "apic/lapic.h"
#include "apic/lapic_timer.h"
#include "console/console.h"
#include "console/debug.h"
#include "console/klog.h"
#include "cpu/features.h"
#include "cpu/gdt.h"
#include "cpu/idt.h"
#include "cpu/tsc.h"
#include "drivers/timer/pit.h"
#include "io/io.h"
#include "lib/string.h"
#include "lib/tsc.h"
#include "mm/pmm.h"
#include "mm/vmm.h"
#include "syscalls/syscall.h"
#include <stdbool.h>
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

/* Before every CPU has installed its own GS base, cpu_get_current() must use
 * RDMSR. This is one global fast-path gate, so it is enabled only after AP
 * startup finishes; setting it in cpu_set_gs_base() would let the BSP's flag
 * make a later-starting AP dereference its still-uninitialized GS base. */
static volatile bool cpu_gs_installed;

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

/* GS base points at this CPU's cpu_info and cpu_info.self (offset 0) points
 * back at itself, so the current CPU struct is one segment-relative load
 * away.  The old rdmsr(MSR_GS_BASE) is a VM exit under KVM (see the matching
 * note in fs/page_cache.c) and this runs on every syscall via
 * sched_get_current() plus every kmalloc/kfree.  Until cpu_set_gs_base() has
 * installed a self-pointing base we must return the raw MSR value exactly as
 * before - see cpu_gs_installed above. */
struct cpu_info *cpu_get_current(void) {
  if (__atomic_load_n(&cpu_gs_installed, __ATOMIC_ACQUIRE)) {
    struct cpu_info *info;
    __asm__ volatile("movq %%gs:0, %0" : "=r"(info));
    return info;
  }
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
  DBG("[AP] entered long mode\n");

  // We are now in 64-bit Long Mode!

  // 0. Load the proper full kernel GDT (replaces the temporary trampoline GDT)
  // This must be done FIRST because gdt_flush zeroes data segments like GS,
  // and we depend on the 64-bit code segment for subsequent interrupt handling.
  // Each AP also loads its own TSS so TSS.RSP0 is private to this core.
  struct cpu_info *bootstrap = (struct cpu_info *)starting_cpu;
  gdt_load_ap(bootstrap ? bootstrap->cpu_id : 0);
  cpu_features_init();
  DBG("[AP] gdt + cpu features ok\n");

  // 1. Setup GS base using the pointer passed by the BSP
  cpu_set_gs_base((struct cpu_info *)starting_cpu);
  struct cpu_info *current = cpu_get_current();

  // 1.5 Switch to the dedicated kernel stack and page tables.
  // This is CRITICAL: APs must be off all bootloader memory (including tables)

  cpu_set_active_cr3(current->kernel_cr3);
  __asm__ volatile("mov %0, %%cr3" ::"r"(current->kernel_cr3) : "memory");
  cpu_switch_stack(current->stack_top);
  DBG("[AP] cpu %u: cr3 + kernel stack ok\n", current->cpu_id);

  // 2. Initialize the LAPIC for this core (needs the HHDM base mapping)
  lapic_init((uint64_t)acpi_get_lapic_base());
  DBG("[AP] cpu %u: lapic ok\n", current->cpu_id);

  // 3. Load the IDT for this core (reuse the BSP's already-built table)
  idt_load();

  // 3.5. Program this core's SYSCALL/SYSRET MSRs.  They are per-CPU: without
  // this, a user thread scheduled here executes `syscall` with EFER.SCE clear
  // and takes a #UD (observed as a SIGILL in radeonsi's compiler threads).
  syscall_init_cpu();

  // 4. initialize the LAPIC timer for this core so it can independently preempt
  lapic_timer_init_ap();
  DBG("[AP] cpu %u: idt + syscall + timer ok\n", current->cpu_id);

  // 5. Enable interrupts locally on this core
  hal_irq_enable();

  // Mark as online to unblock the BSP's boot loop
  __atomic_store_n(&current->status, CPU_STATUS_ONLINE, __ATOMIC_RELEASE);

  klog_puts("     AP Woke up! CPU ");
  klog_uint64(current->cpu_id);
  klog_puts(" (APIC ID ");
  klog_hex32(current->apic_id);
  klog_puts(") ONLINE.\n");
  DBG("[AP] cpu %u: ONLINE, entering idle\n", current->cpu_id);

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
    /* Deferred address-space teardown (execve / process reaper) drains here:
     * this core would be halted anyway, so turning a dead process's page
     * tables into free frames costs nothing.  While a backlog exists, tick at
     * 1 ms (the same trick as the BSP's serial-pending idle tick) so the
     * drain outruns producers; otherwise keep the 1 s tick for hang
     * detection. */
    vmm_defer_drain(VMM_DEFER_IDLE_NODES);
    lapic_timer_rearm_if_earlier(lapic_timer_get_ms() +
                                 (vmm_defer_pending() ? 1 : 1000));
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
  // The BSP is running on the kernel CR3 right now; APs publish their own
  // when they load it in ap_main().
  cpus[0].active_cr3 = cr3;

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

// ── AP startup ──────────────────────────────────────────────────────────────
//
// These helpers run before any AP is online, so they must not rely on IPIs,
// the scheduler, or the LAPIC timer (which may not have calibrated).  Delays
// are TSC busy-waits, paced with I/O port writes only if the TSC frequency is
// unknown.

static void ap_delay_us(uint32_t us) {
  uint64_t khz = tsc_get_freq_khz();
  if (khz != 0) {
    uint64_t cycles = (khz * (uint64_t)us) / 1000;
    uint64_t start = rdtsc();
    while (rdtsc() - start < cycles)
      hal_cpu_relax();
    return;
  }
  // No TSC timebase.  Port 0x80 is the legacy POST port; each write is a
  // few hundred nanoseconds on real hardware.  Crude, but bounded.
  for (uint64_t i = 0; i < (uint64_t)us * 10; i++)
    outb(0x80, 0);
}

static bool ap_wait_icr_idle(uint32_t timeout_us) {
  uint64_t khz = tsc_get_freq_khz();
  if (khz != 0) {
    uint64_t cycles = (khz * (uint64_t)timeout_us) / 1000;
    uint64_t start = rdtsc();
    while (rdtsc() - start < cycles) {
      if (lapic_icr_idle())
        return true;
      hal_cpu_relax();
    }
  } else {
    for (uint64_t i = 0; i < (uint64_t)timeout_us * 100; i++) {
      if (lapic_icr_idle())
        return true;
      hal_cpu_relax();
    }
  }
  return lapic_icr_idle();
}

static bool ap_wait_online(struct cpu_info *ap, uint32_t timeout_ms) {
  uint64_t khz = tsc_get_freq_khz();
  if (khz != 0) {
    uint64_t cycles = khz * (uint64_t)timeout_ms;
    uint64_t start = rdtsc();
    while (__atomic_load_n(&ap->status, __ATOMIC_ACQUIRE) !=
           CPU_STATUS_ONLINE) {
      if (rdtsc() - start >= cycles)
        break;
      hal_cpu_relax();
    }
  } else {
    for (uint64_t i = 0; i < (uint64_t)timeout_ms * 200000; i++) {
      if (__atomic_load_n(&ap->status, __ATOMIC_ACQUIRE) == CPU_STATUS_ONLINE)
        break;
      hal_cpu_relax();
    }
  }
  return __atomic_load_n(&ap->status, __ATOMIC_ACQUIRE) == CPU_STATUS_ONLINE;
}

// Put a CPU back into wait-for-SIPI state.  Called when an AP did not come
// online so it cannot wake up later and consume the shared trampoline baton
// that the next AP is about to reuse.
static void ap_park(uint32_t apic_id) {
  lapic_write_icr(apic_id, LAPIC_ICR_INIT | LAPIC_ICR_LEVEL | LAPIC_ICR_ASSERT);
  (void)ap_wait_icr_idle(1000);
  ap_delay_us(10000);
  lapic_write_icr(apic_id, LAPIC_ICR_INIT | LAPIC_ICR_LEVEL | LAPIC_ICR_DEASSERT);
  (void)ap_wait_icr_idle(1000);
}

// Returns true when the AP reached CPU_STATUS_ONLINE.
static bool ap_start(struct cpu_info *ap, volatile uint64_t *ptr_stack,
                     uint8_t sipi_vector) {
  *ptr_stack = ap->stack_top;
  starting_cpu = ap;

  DBG("[SMP] AP %u (APIC 0x%x): INIT assert\n", ap->cpu_id, ap->apic_id);

  // Intel SDM 8.4.4: INIT is a level-triggered assertion followed by a
  // deassert.  The old sequence sent an edge-encoded INIT and a single
  // STARTUP; some real chipsets ignore that INIT and then drop the SIPI,
  // leaving the BSP spinning in the old unbounded ONLINE loop forever.
  lapic_write_icr(ap->apic_id,
                  LAPIC_ICR_INIT | LAPIC_ICR_LEVEL | LAPIC_ICR_ASSERT);
  if (!ap_wait_icr_idle(1000))
    DBG("[SMP] AP %u: INIT delivery status stuck busy\n", ap->cpu_id);
  ap_delay_us(10000);

  lapic_write_icr(ap->apic_id,
                  LAPIC_ICR_INIT | LAPIC_ICR_LEVEL | LAPIC_ICR_DEASSERT);
  (void)ap_wait_icr_idle(1000);
  ap_delay_us(200);

  // Send STARTUP twice: the first SIPI can be lost while the AP is still
  // finishing its reset microcode.
  for (int attempt = 0; attempt < 2; attempt++) {
    DBG("[SMP] AP %u: SIPI #%d -> vector 0x%x\n", ap->cpu_id, attempt + 1,
        sipi_vector);
    lapic_write_icr(ap->apic_id,
                    LAPIC_ICR_STARTUP | LAPIC_ICR_EDGE | sipi_vector);
    if (!ap_wait_icr_idle(1000))
      DBG("[SMP] AP %u: SIPI #%d delivery status stuck busy\n", ap->cpu_id,
          attempt + 1);
    ap_delay_us(200);
    if (__atomic_load_n(&ap->status, __ATOMIC_ACQUIRE) == CPU_STATUS_ONLINE) {
      DBG("[SMP] AP %u: online\n", ap->cpu_id);
      return true;
    }
  }

  if (ap_wait_online(ap, 1000)) {
    DBG("[SMP] AP %u: online (late)\n", ap->cpu_id);
    return true;
  }

  // Park the AP before the caller reuses the trampoline for the next CPU.
  DBG("[SMP] AP %u: no response after 2 SIPIs; parking and continuing\n",
      ap->cpu_id);
  ap_park(ap->apic_id);

  console_puts(KLOG_CLR_RED "[ FAIL ]" KLOG_CLR_RESET " CPU ");
  print_uint32(ap->cpu_id);
  console_puts(" (APIC ID 0x");
  print_hex32(ap->apic_id);
  console_puts(") did not come online.\n");
  return false;
}

void cpu_init_aps(void) {
  // Step 7: Wake up the Application Processors
  if (cpu_count > 1) {
    DBG("[SMP] cpu_init_aps: entering bring-up\n");
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

    uint8_t vector = tramp_phys >> 12; // 0x08

    DBG("[SMP] %u AP(s) to start; LAPIC mode: %s; trampoline at 0x%x\n",
        cpu_count - 1, lapic_is_x2apic() ? "x2APIC (MSR)" : "xAPIC (MMIO)",
        (unsigned)tramp_phys);

    uint32_t online = 1; // the BSP
    uint32_t failed = 0;

    for (uint32_t i = 1; i < cpu_count; i++) {
      if (cpus[i].status == CPU_STATUS_BSP)
        continue;

      if (ap_start(&cpus[i], ptr_stack, vector))
        online++;
      else
        failed++;
    }

    if (failed > 0) {
      console_puts(KLOG_CLR_YELLOW "[ WARN ]" KLOG_CLR_RESET " ");
      print_uint32(failed);
      console_puts(" Application Processor(s) failed to start; continuing with ");
      print_uint32(online);
      console_puts(" CPU(s).\n");
    }
  }

  /* The BSP and every AP that successfully started have their own GS base.
   * Keep using RDMSR during bootstrap, then publish the safe GS:0 fast path
   * once no additional AP can still be running with the trampoline GS state. */
  __atomic_store_n(&cpu_gs_installed, true, __ATOMIC_RELEASE);
}
