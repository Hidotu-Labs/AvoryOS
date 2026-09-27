#include "isr.h"
#include "../console/console.h"
#include "../console/klog.h"
#include "../mm/pmm.h"
#include "../mm/vma.h"
#include "../mm/vmm.h"
#include "../sched/sched.h"
#include "../socket/socket.h"
#include "../syscalls/syscall.h"
#include "apic/lapic.h"
#include "arch/x86_64/extable.h"
#include "fault.h"
#include "features.h"
#include "fpu.h"
#include "kpf_dump.h"
#include "ktrack.h"
#include "msr.h"
#include "pic.h"
#include "../drivers/serial.h"

const char *exception_messages[] = {"Division By Zero",
                                    "Debug",
                                    "Non Maskable Interrupt",
                                    "Breakpoint",
                                    "Into Detected Overflow",
                                    "Out of Bounds",
                                    "Invalid Opcode",
                                    "No Coprocessor",
                                    "Double Fault",
                                    "Coprocessor Segment Overrun",
                                    "Bad TSS",
                                    "Segment Not Present",
                                    "Stack Fault",
                                    "General Protection Fault",
                                    "Page Fault",
                                    "Unknown Interrupt",
                                    "Coprocessor Fault",
                                    "Alignment Check",
                                    "Machine Check",
                                    "SIMD Floating-Point Exception",
                                    "Virtualization Exception",
                                    "Control Protection Exception",
                                    "Reserved",
                                    "Reserved",
                                    "Reserved",
                                    "Reserved",
                                    "Reserved",
                                    "Reserved",
                                    "Hypervisor Injection Exception",
                                    "VMM Communication Exception",
                                    "Security Exception",
                                    "Reserved"};

// Low-level output helpers

static void print_hex(uint64_t value) {
  const char *hex_chars = "0123456789ABCDEF";
  console_puts("0x");
  for (int i = 15; i >= 0; i--) {
    console_putchar(hex_chars[(value >> (i * 4)) & 0xF]);
  }
}

static void print_hex8(uint8_t value) {
  const char *hex_chars = "0123456789ABCDEF";
  console_puts("0x");
  console_putchar(hex_chars[(value >> 4) & 0xF]);
  console_putchar(hex_chars[value & 0xF]);
}

static void print_dec(uint64_t value) {
  if (value == 0) {
    console_putchar('0');
    return;
  }
  char buf[21];
  int i = 0;
  while (value > 0) {
    buf[i++] = '0' + (value % 10);
    value /= 10;
  }
  for (int j = i - 1; j >= 0; j--)
    console_putchar(buf[j]);
}

static void print_reg_line(const char *name, uint64_t value) {
  console_puts(name);
  console_puts(": ");
  print_hex(value);
  console_puts("\n");
}

static void print_yes_no(const char *name, bool value) {
  console_puts(name);
  console_puts("=");
  console_puts(value ? "1 " : "0 ");
}

static bool is_canonical_addr(uint64_t vaddr) {
  uint64_t high = vaddr >> 48;
  return (high == 0x0000ULL) || (high == 0xFFFFULL);
}

// RFLAGS decoder

static void print_rflags_decoded(uint64_t rflags) {
  console_puts("RFLAGS: ");
  print_hex(rflags);
  console_puts("\n  Flags: ");
  print_yes_no("CF", (rflags >> 0) & 1);
  print_yes_no("PF", (rflags >> 2) & 1);
  print_yes_no("AF", (rflags >> 4) & 1);
  print_yes_no("ZF", (rflags >> 6) & 1);
  print_yes_no("SF", (rflags >> 7) & 1);
  print_yes_no("TF", (rflags >> 8) & 1);
  print_yes_no("IF", (rflags >> 9) & 1);
  print_yes_no("DF", (rflags >> 10) & 1);
  print_yes_no("OF", (rflags >> 11) & 1);
  console_puts("\n  IOPL=");
  print_dec((rflags >> 12) & 0x3);
  print_yes_no(" NT", (rflags >> 14) & 1);
  print_yes_no("RF", (rflags >> 16) & 1);
  print_yes_no("VM", (rflags >> 17) & 1);
  print_yes_no("AC", (rflags >> 18) & 1);
  print_yes_no("VIF", (rflags >> 19) & 1);
  print_yes_no("VIP", (rflags >> 20) & 1);
  print_yes_no("ID", (rflags >> 21) & 1);
  console_puts("\n");
}

// Control register diagnostics

static void print_cr_state(void) {
  uint64_t cr0, cr3, cr4;
  __asm__ volatile("mov %%cr0, %0" : "=r"(cr0));
  __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
  __asm__ volatile("mov %%cr4, %0" : "=r"(cr4));

  console_puts("\nCONTROL REGISTERS:\n");

  console_puts("CR0: ");
  print_hex(cr0);
  console_puts("\n  ");
  print_yes_no("PE", (cr0 >> 0) & 1);
  print_yes_no("WP", (cr0 >> 16) & 1);
  print_yes_no("PG", (cr0 >> 31) & 1);
  console_puts("\n");

  console_puts("CR3: ");
  print_hex(cr3);
  console_puts("\n  PCID=");
  print_hex(cr3 & 0xFFF);
  console_puts(" PML4_PHYS=");
  print_hex(cr3 & ~0xFFFULL);
  console_puts("\n");

  console_puts("CR4: ");
  print_hex(cr4);
  console_puts("\n  ");
  print_yes_no("PSE", (cr4 >> 4) & 1);
  print_yes_no("PAE", (cr4 >> 5) & 1);
  print_yes_no("PGE", (cr4 >> 7) & 1);
  print_yes_no("SMEP", (cr4 >> 20) & 1);
  print_yes_no("SMAP", (cr4 >> 21) & 1);
  print_yes_no("PKE", (cr4 >> 22) & 1);
  print_yes_no("CET", (cr4 >> 23) & 1);
  console_puts("\n");
}

// GP fault decoder

static void print_gp_error_details(uint64_t err_code) {
  console_puts("GP_ERR_DETAILS: ");
  if (err_code == 0) {
    console_puts("none (not selector-related)\n");
    return;
  }
  const char *tbl_names[] = {"GDT", "IDT", "LDT", "IDT"};
  uint8_t ext = err_code & 0x1;
  uint8_t tbl = (err_code >> 1) & 0x3;
  uint16_t idx = (err_code >> 3) & 0x1FFF;
  console_puts("ext=");
  print_hex8(ext);
  console_puts(" tbl=");
  console_puts(tbl_names[tbl]);
  console_puts(" idx=");
  print_dec(idx);
  console_puts(" (");
  print_hex(err_code);
  console_puts(")\n");
}

// PF fault error code decoder

static void print_pf_error_details(uint64_t err_code) {
  bool p = (err_code >> 0) & 1;    /* page present */
  bool w = (err_code >> 1) & 1;    /* write */
  bool u = (err_code >> 2) & 1;    /* user mode */
  bool rsvd = (err_code >> 3) & 1; /* reserved bit set in PTE */
  bool i = (err_code >> 4) & 1;    /* instruction fetch */
  bool pk = (err_code >> 5) & 1;   /* protection key */
  bool ss = (err_code >> 6) & 1;   /* shadow stack */

  console_puts("PF_ERR_DETAILS: ");
  print_yes_no("P", p);
  print_yes_no("W", w);
  print_yes_no("U", u);
  print_yes_no("RSVD", rsvd);
  print_yes_no("I", i);
  print_yes_no("PK", pk);
  print_yes_no("SS", ss);
  console_puts("\n  Cause: ");
  if (i)
    console_puts("instruction fetch from ");
  else if (w)
    console_puts("write to ");
  else
    console_puts("read from ");
  console_puts(p ? "present page (protection violation)" : "non-present page");
  if (u)
    console_puts(", from user mode");
  else
    console_puts(", from kernel mode");
  if (rsvd)
    console_puts(" [RSVD BIT IN PTE - possible corruption]");
  if (pk)
    console_puts(" [protection-key violation]");
  if (ss)
    console_puts(" [shadow stack violation]");
  console_puts("\n");
}

static void analyze_pte_corruption(uint64_t pte) {
  const uint64_t PA_MASK = 0x000FFFFFFFFFF000ULL;
  const uint64_t LOW_MASK = 0xFFFULL;               /* bits [11:0] */
  const uint64_t HIGH_MASK = 0x7FF0000000000000ULL; /* bits [62:52] */

  uint64_t low_bits = pte & LOW_MASK;
  uint64_t high_bits = pte & HIGH_MASK;
  uint64_t pa = pte & PA_MASK;

  bool present = (pte >> 0) & 1;
  bool rw = (pte >> 1) & 1;
  bool us = (pte >> 2) & 1;
  bool dirty = (pte >> 6) & 1;
  bool global = (pte >> 8) & 1;
  bool nx = (pte >> 63) & 1;

  console_puts("  PTE_CORRUPTION_ANALYSIS:\n");
  console_puts("    raw=");
  print_hex(pte);
  console_puts("\n    PA field=");
  print_hex(pa);
  console_puts("\n    low_bits[11:0]=");
  print_hex(low_bits);
  console_puts(" high_bits[62:52]=");
  print_hex(high_bits);
  console_puts("\n");

  if (high_bits != 0) {
    console_puts("    WARN: bits [62:52] are non-zero (");
    print_hex(high_bits);
    console_puts(") -- reserved or OS-specific bits set; likely corruption\n");
  }

  uint32_t lo32 = (uint32_t)(pte & 0xFFFFFFFFULL);
  uint32_t hi32 = (uint32_t)(pte >> 32);
  if (hi32 != 0 && hi32 != 0xFFFFFFFF) {
    uint32_t diff = lo32 ^ hi32;
    if (__builtin_popcount(diff) <= 4) {
      console_puts("    WARN: upper and lower 32-bit halves differ by only ");
      print_dec(__builtin_popcount(diff));
      console_puts(" bit(s) (lo=");
      print_hex(lo32);
      console_puts(" hi=");
      print_hex(hi32);
      console_puts(
          ") -- looks like a 32-bit value replicated into both halves\n");
    }
  }

  if (!present && dirty)
    console_puts("    WARN: D=1 but P=0 (dirty non-present page)\n");
  if (!present && global)
    console_puts("    WARN: G=1 but P=0 (global non-present page)\n");
  if (!rw && dirty)
    console_puts("    NOTE: D=1 but RW=0 (was writable, now read-only)\n");
  if (nx && !us && !present)
    console_puts("    NOTE: NX + kernel + non-present\n");

  if (high_bits == 0 && lo32 == 0 && hi32 == 0)
    console_puts("    PTE is completely zero (was never mapped or was "
                 "explicitly cleared)\n");
}

static void print_paging_entry_flags(uint64_t entry, bool is_leaf,
                                     bool is_pde) {
  print_yes_no("P", (entry & (1ULL << 0)) != 0);
  print_yes_no("RW", (entry & (1ULL << 1)) != 0);
  print_yes_no("US", (entry & (1ULL << 2)) != 0);
  print_yes_no("PWT", (entry & (1ULL << 3)) != 0);
  print_yes_no("PCD", (entry & (1ULL << 4)) != 0);
  print_yes_no("A", (entry & (1ULL << 5)) != 0);
  if (is_leaf || is_pde)
    print_yes_no("D", (entry & (1ULL << 6)) != 0);
  if (is_pde)
    print_yes_no("PS", (entry & (1ULL << 7)) != 0);
  else if (is_leaf)
    print_yes_no("PAT", (entry & (1ULL << 7)) != 0);
  if (is_leaf)
    print_yes_no("G", (entry & (1ULL << 8)) != 0);
  print_yes_no("NX", (entry & (1ULL << 63)) != 0);
  console_puts("\n");
}

static void print_entry_summary(const char *name, size_t idx, uint64_t entry,
                                bool is_leaf, bool is_pde) {
  console_puts("  ");
  console_puts(name);
  console_puts("[");
  print_hex(idx);
  console_puts("] raw=");
  print_hex(entry);
  console_puts(" phys_base=");
  print_hex(entry & PAGE_MASK);
  console_puts("\n    flags: ");
  print_paging_entry_flags(entry, is_leaf, is_pde);
}

static void print_neighbor_entries(const char *label, uint64_t *table,
                                   size_t index) {
  size_t start = (index > 1) ? index - 1 : 0;
  size_t end = (index < 510) ? index + 1 : 511;

  console_puts("    ");
  console_puts(label);
  console_puts(" neighborhood:\n");
  for (size_t i = start; i <= end; i++) {
    console_puts("      [");
    print_hex(i);
    console_puts("] = ");
    print_hex(table[i]);
    if (i == index)
      console_puts("  <target>");
    console_puts("\n");
  }
}

static void print_pf_walk(uint64_t cr2) {
  uint64_t hhdm = pmm_get_hhdm_offset();
  uint64_t *pml4_phys = vmm_get_active_pml4();
  uint64_t *pml4 = (uint64_t *)((uint64_t)pml4_phys + hhdm);

  size_t pml4_i = (cr2 >> 39) & 0x1FF;
  size_t pdpt_i = (cr2 >> 30) & 0x1FF;
  size_t pd_i = (cr2 >> 21) & 0x1FF;
  size_t pt_i = (cr2 >> 12) & 0x1FF;
  uint64_t page_off = cr2 & 0xFFF;

  console_puts("PF_WALK:\n");
  uint64_t pml4e = pml4[pml4_i];
  print_entry_summary("PML4E", pml4_i, pml4e, false, false);
  if (!(pml4e & 1))
    return;

  uint64_t *pdpt = (uint64_t *)((pml4e & PAGE_MASK) + hhdm);
  uint64_t pdpte = pdpt[pdpt_i];
  print_entry_summary("PDPTE", pdpt_i, pdpte, false, false);
  if (!(pdpte & 1) || (pdpte & (1ULL << 7)))
    return;

  uint64_t *pd = (uint64_t *)((pdpte & PAGE_MASK) + hhdm);
  uint64_t pde = pd[pd_i];
  print_entry_summary("PDE", pd_i, pde, false, true);
  if (!(pde & 1) || (pde & (1ULL << 7)))
    return;

  uint64_t *pt = (uint64_t *)((pde & PAGE_MASK) + hhdm);
  uint64_t pte = pt[pt_i];
  print_entry_summary("PTE", pt_i, pte, true, false);
}

static void print_user_stack_words(uint64_t user_rsp, int words) {
  uint64_t *pml4 = vmm_get_active_pml4();
  console_puts("USER_STACK_TOP:\n");
  for (int i = 0; i < words; i++) {
    uint64_t addr = user_rsp + ((uint64_t)i * sizeof(uint64_t));
    console_puts("  [");
    print_hex(addr);
    console_puts("] = ");
    if (vmm_virt_to_phys(pml4, addr) == 0) {
      console_puts("<unmapped>\n");
      continue;
    }
    print_hex(*(volatile uint64_t *)addr);
    console_puts("\n");
  }
}

static void print_context_summary(struct registers *regs) {
  uint8_t cpl = regs->cs & 0x3;
  console_puts("CONTEXT: ");
  if (cpl == 0)
    console_puts("kernel (ring 0)");
  else
    console_puts("user   (ring 3)");
  console_puts("  CS=");
  print_hex(regs->cs);
  console_puts("  SS=");
  print_hex(regs->ss);
  console_puts("\n");
}

static isr_t interrupt_handlers[256] = {0};
static bool apic_mode = false;
static bool allocated_device_vectors[256] = {0};

void isr_set_apic_mode(bool enabled) { apic_mode = enabled; }

void register_interrupt_handler(uint8_t n, isr_t handler) {
  interrupt_handlers[n] = handler;
}

int interrupt_vector_alloc(isr_t handler) {
  if (!handler) return -1;
  /* 0x60..0xDF avoids exceptions, legacy IRQs, timer/IPIs and spurious. */
  for (uint16_t vector = 0x60; vector <= 0xDF; vector++) {
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

// Send End-of-Interrupt signal after handler completes.
// CRITICAL: This must be called AFTER the handler returns, not during.
// For shared IRQs (multiple handlers on one IRQ), this ensures all handlers
// run before we tell the APIC we're done. If EOI were sent mid-dispatch,
// the APIC would accept a new interrupt before all shared handlers complete,
// potentially causing handlers to miss their turn or see corrupted state.
static void send_eoi(struct registers *regs) {
  if (regs->int_no == 255)
    return;
  if (regs->int_no < 32)
    return;
  if (apic_mode) {
    /* Bring-up diagnostic: prove the LAPIC page is reachable in the active
     * address space before the store faults on it.  serial_write_sync() does
     * not take klog locks, so this is safe even when the interrupt landed in
     * the middle of a klog print (which is exactly when the missing mapping
     * used to show up). */
    uint64_t lapic_va = lapic_get_va();
    if (lapic_va) {
      uint64_t cr3;
      __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
      uint64_t cr3_base = cr3 & PAGE_MASK;
      if (!vmm_debug_walk(cr3_base, lapic_va, NULL)) {
        static volatile unsigned eoi_reported;
        if (!__atomic_exchange_n(&eoi_reported, 1, __ATOMIC_ACQ_REL)) {
          vmm_debug_dump_walk("eoi-missing active", cr3_base, lapic_va);
          vmm_debug_dump_walk("eoi-missing kernel",
                              (uint64_t)(uintptr_t)vmm_get_kernel_pml4(),
                              lapic_va);
        }
      }
    }
    lapic_send_eoi();
  } else if (regs->int_no <= 47) {
    pic_send_eoi(regs->int_no - 32);
  }
}

// Exception Handling & Signals

static void isr_panic(struct registers *regs, const char *msg) {
  /* Announce the panic on the serial line before touching the console: the
   * framebuffer path takes locks this CPU may already be holding, and the
   * console dump never reaches the log if that hangs. */
  kpf_dump_panic_entry(msg, regs);

  /* A panic that faults on the way out - the console dump touches the
   * framebuffer, locks and the faulting thread's stack - used to re-enter this
   * function and start the whole dump again, forever. That loop is what looks
   * like a hang on the serial line. Give up on the console after the first
   * attempt and stop cleanly instead. */
  static volatile int panicking;
  if (__atomic_exchange_n(&panicking, 1, __ATOMIC_ACQ_REL)) {
    kpf_dump_panic_recursion(msg, regs);
    for (;;) {
      __asm__ volatile("cli; hlt");
    }
  }

  console_clear();
  console_puts("==================== KERNEL PANIC ====================\n");
  console_puts(msg);
  console_puts("  [INT ");
  print_dec(regs->int_no);
  console_puts("]\n");

  if (regs->int_no < 32) {
    console_puts("Exception: ");
    console_puts(exception_messages[regs->int_no]);
    console_puts("\n");
  }

  console_puts("ERR_CODE: ");
  print_hex(regs->err_code);
  console_puts("\n\n");

  print_context_summary(regs);

  // RIP is always valid (CPU saves it for all exceptions).
  // RSP/SS are only pushed by the CPU on a privilege-level change (ring-3 →
  // ring-0). For ring-0 exceptions, regs->rsp and regs->ss are garbage from
  // adjacent stack memory.  In that case, grab a live RSP snapshot via inline
  // asm — it won't be the exact pre-fault RSP, but it's in the right ballpark.
  uint8_t cpl = regs->cs & 0x3;
  uint64_t display_rsp;
  if (cpl == 0) {
    __asm__ volatile("mov %%rsp, %0" : "=r"(display_rsp));
    console_puts("RIP: ");
    print_hex(regs->rip);
    console_puts("\nRSP: ");
    print_hex(display_rsp);
    console_puts(" (live snapshot; fault RSP not saved by CPU for ring-0)\n");
  } else {
    console_puts("RIP: ");
    print_hex(regs->rip);
    console_puts(" RSP: ");
    print_hex(regs->rsp);
    console_puts("\n");
  }

  // General-purpose registers
  console_puts("\nGENERAL PURPOSE REGISTERS:\n");
  print_reg_line("  RAX", regs->rax);
  print_reg_line("  RBX", regs->rbx);
  print_reg_line("  RCX", regs->rcx);
  print_reg_line("  RDX", regs->rdx);
  print_reg_line("  RSI", regs->rsi);
  print_reg_line("  RDI", regs->rdi);
  print_reg_line("  RBP", regs->rbp);
  print_reg_line("  R8 ", regs->r8);
  print_reg_line("  R9 ", regs->r9);
  print_reg_line("  R10", regs->r10);
  print_reg_line("  R11", regs->r11);
  print_reg_line("  R12", regs->r12);
  print_reg_line("  R13", regs->r13);
  print_reg_line("  R14", regs->r14);
  print_reg_line("  R15", regs->r15);

  print_rflags_decoded(regs->rflags);

  if (regs->int_no == 13) {
    print_gp_error_details(regs->err_code);
  } else if (regs->int_no == 14) {
    uint64_t cr2;
    __asm__ volatile("mov %%cr2, %0" : "=r"(cr2));
    console_puts("CR2: ");
    print_hex(cr2);
    console_puts("\n");
    print_pf_error_details(regs->err_code);
    print_pf_walk(cr2);
  }

  print_cr_state();

  console_puts("\nSystem Halted.\n");
  serial_flush_sync();
  for (;;) {
    __asm__ volatile("cli; hlt");
  }
}

static void klog_print_yes_no(const char *name, bool value) {
  klog_puts(name);
  klog_puts(value ? "=1 " : "=0 ");
}

static void klog_rflags_decoded(uint64_t rflags) {
  klog_puts("RFLAGS: ");
  klog_hex64(rflags);
  klog_puts("\n  [ ");
  klog_print_yes_no("CF", (rflags >> 0) & 1);
  klog_print_yes_no("PF", (rflags >> 2) & 1);
  klog_print_yes_no("AF", (rflags >> 4) & 1);
  klog_print_yes_no("ZF", (rflags >> 6) & 1);
  klog_print_yes_no("SF", (rflags >> 7) & 1);
  klog_print_yes_no("TF", (rflags >> 8) & 1);
  klog_print_yes_no("IF", (rflags >> 9) & 1);
  klog_print_yes_no("DF", (rflags >> 10) & 1);
  klog_print_yes_no("OF", (rflags >> 11) & 1);
  klog_puts("IOPL=");
  klog_uint64((rflags >> 12) & 0x3);
  klog_puts(" ");
  klog_print_yes_no("NT", (rflags >> 14) & 1);
  klog_print_yes_no("RF", (rflags >> 16) & 1);
  klog_print_yes_no("VM", (rflags >> 17) & 1);
  klog_print_yes_no("AC", (rflags >> 18) & 1);
  klog_print_yes_no("VIF", (rflags >> 19) & 1);
  klog_print_yes_no("VIP", (rflags >> 20) & 1);
  klog_print_yes_no("ID", (rflags >> 21) & 1);
  klog_puts("]\n");
}

static void klog_dump_ptr(const char *reg_name, uint64_t val) {
    if (val < 0x10000) return; // likely small constant or null
    if (val >= 0x0000800000000000ULL && val < 0xFFFF800000000000ULL) return; // non-canonical
    if (val >= 0xFFFF800000000000ULL) return; // kernel address (don't dump from user fault)
    
    uint64_t *pml4 = vmm_get_active_pml4();
    uint64_t phys = vmm_virt_to_phys(pml4, val);
    if (phys != 0) {
        klog_puts("  *"); klog_puts(reg_name); klog_puts(" ("); klog_hex64(val); klog_puts("): ");
        uint64_t hhdm_addr = phys + pmm_get_hhdm_offset();
        // Dump 32 bytes or until page boundary
        uint64_t offset_in_page = val & 0xFFF;
        uint32_t to_dump = 32;
        if (offset_in_page + to_dump > 0x1000) to_dump = 0x1000 - offset_in_page;
        
        uint8_t *ptr = (uint8_t*)hhdm_addr;
        const char *_h = "0123456789ABCDEF";
        for (uint32_t i = 0; i < to_dump; i++) {
            klog_putchar(_h[(ptr[i] >> 4) & 0xF]);
            klog_putchar(_h[ptr[i] & 0xF]);
            klog_putchar(' ');
        }
        klog_puts("\n");
    }
}

static const char *get_signal_name(int sig) {
  switch (sig) {
    case 1: return "SIGHUP";
    case 2: return "SIGINT";
    case 3: return "SIGQUIT";
    case 4: return "SIGILL";
    case 5: return "SIGTRAP";
    case 6: return "SIGABRT";
    case 7: return "SIGBUS";
    case 8: return "SIGFPE";
    case 9: return "SIGKILL";
    case 10: return "SIGUSR1";
    case 11: return "SIGSEGV";
    case 12: return "SIGUSR2";
    case 13: return "SIGPIPE";
    case 14: return "SIGALRM";
    case 15: return "SIGTERM";
    case 16: return "SIGSTKFLT";
    case 31: return "SIGSYS";
    default: return "UNKNOWN";
  }
}

void isr_report_user_fault(struct registers *regs, int sig,
                           uint64_t addr) {
  struct thread *current = sched_get_current();
  if (current) {
    bool has_custom_handler = false;
    if (sig >= 1 && sig <= 64) {
      void *handler = (void *)current->signal_handlers[sig - 1].sa_handler;
      if (handler != NULL && handler != (void *)1) {
        has_custom_handler = true;
      }
    }

    if (!has_custom_handler) {
      if (current->last_report_sig == sig &&
          current->last_report_rip == regs->rip &&
          current->last_report_addr == addr) {
        return;
      }
      current->last_report_sig = sig;
      current->last_report_rip = regs->rip;
      current->last_report_addr = addr;
      // 1. Log detailed report to kernel console
      klog_puts("\n" KLOG_CLR_RED "################################################################################" KLOG_CLR_RESET "\n");
      klog_puts(KLOG_CLR_RED "[ USER FAULT ]" KLOG_CLR_RESET " process '");
      klog_puts(current->comm);
      klog_puts("' (tid=");
      klog_uint64(current->tid);
      klog_puts(", tgid=");
      klog_uint64(current->tgid);
      klog_puts(") signal ");
      klog_uint64(sig);
      klog_puts(" (");
      klog_puts(get_signal_name(sig));
      klog_puts(")\n");
      
      // Fault Reason
      klog_puts("  Fault Reason: ");
      if (regs->int_no == 3) {
        klog_puts(KLOG_CLR_YELLOW "Breakpoint instruction (int3)" KLOG_CLR_RESET);
      } else if (regs->int_no == 13) {
        /* #GP error codes carry a selector, not page bits: bit 1 = IDT,
         * bit 2 = LDT (else GDT), bits 3-15 = selector index.  Without this
         * decode a DPL violation on an IDT gate looks like an NX fault,
         * because the index lands in bit 4. */
        uint64_t gerr = regs->err_code;
        if (gerr == 0) {
          klog_puts(KLOG_CLR_YELLOW "General protection fault" KLOG_CLR_RESET);
        } else {
          klog_puts(KLOG_CLR_YELLOW "#GP selector error: index " KLOG_CLR_RESET);
          klog_uint64(gerr >> 3);
          klog_puts(KLOG_CLR_YELLOW " in " KLOG_CLR_RESET);
          klog_puts((gerr & 2) ? "IDT" : ((gerr & 4) ? "LDT" : "GDT"));
          if (gerr & 1)
            klog_puts(KLOG_CLR_YELLOW " (external)" KLOG_CLR_RESET);
        }
      } else if (sig == 11) {
        if (addr < 0x1000) {
          klog_puts(KLOG_CLR_YELLOW "Null / near-null pointer dereference" KLOG_CLR_RESET);
        } else if (addr >= 0x0000800000000000ULL && addr < 0xFFFF800000000000ULL) {
          klog_puts(KLOG_CLR_YELLOW "Non-canonical memory access" KLOG_CLR_RESET);
        } else if ((regs->err_code & 1) && (regs->err_code & 16)) {
          /* NX requires a present entry (P=1) plus an instruction fetch. */
          klog_puts(KLOG_CLR_YELLOW "Execute non-executable page (NX)" KLOG_CLR_RESET);
        } else if (regs->err_code & 16) {
          klog_puts(KLOG_CLR_YELLOW "Instruction fetch on an unmapped page" KLOG_CLR_RESET);
        } else if (regs->err_code & 1) {
          klog_puts(KLOG_CLR_YELLOW "Page protection violation (page present)" KLOG_CLR_RESET);
        } else {
          klog_puts(KLOG_CLR_YELLOW "Page fault on unmapped address" KLOG_CLR_RESET);
        }
      } else if (sig == 4) {
        klog_puts(KLOG_CLR_YELLOW "Illegal instruction / invalid opcode" KLOG_CLR_RESET);
      } else if (sig == 8) {
        klog_puts(KLOG_CLR_YELLOW "Floating-point / arithmetic exception" KLOG_CLR_RESET);
      } else if (sig == 7) {
        klog_puts(KLOG_CLR_YELLOW "Bus error / misaligned memory access" KLOG_CLR_RESET);
      } else if (sig == 16) {
        klog_puts(KLOG_CLR_YELLOW "Stack fault" KLOG_CLR_RESET);
      } else {
        klog_puts(KLOG_CLR_YELLOW "Signal delivered to thread" KLOG_CLR_RESET);
      }
      klog_puts("\n");

      klog_puts("  RIP: "); klog_hex64(regs->rip);
      if (current->mm) {
        struct vma *rvma = vma_find(&current->mm->vmas, regs->rip);
        if (rvma) {
          klog_puts(" (VMA: "); klog_hex64(rvma->start); klog_puts(" - "); klog_hex64(rvma->end);
          klog_puts(", offset +"); klog_hex64(regs->rip - rvma->start); klog_puts(")");
        }
      }
      klog_puts("\n");
      klog_puts(regs->int_no == 14 ? "  CR2: " : "  ADDR: ");
      klog_hex64(addr);
      klog_puts("  ERR: "); klog_hex64(regs->err_code);
      klog_puts("  CS: "); klog_hex64(regs->cs);
      klog_puts("\n");

      // Subsystem context
      if (current->last_subsystem || current->last_kernel_file) {
        klog_puts(KLOG_CLR_CYAN "LAST KERNEL SUBSYSTEM CONTEXT:\n" KLOG_CLR_RESET);
        klog_puts("  Subsystem:  [");
        klog_puts(current->last_subsystem ? current->last_subsystem : "UNKNOWN");
        klog_puts("]\n");
        klog_puts("  Location:   ");
        klog_puts(current->last_kernel_file ? current->last_kernel_file : "<unknown>");
        klog_puts(":");
        klog_uint64(current->last_kernel_line);
        if (current->last_kernel_func) {
          klog_puts(" (");
          klog_puts(current->last_kernel_func);
          klog_puts(")");
        }
        klog_puts("\n");
        if (current->last_error_code != 0) {
          klog_puts("  Last Error: ");
          klog_int64(current->last_error_code);
          klog_puts("\n");
        }
      }

      // Syscall context
      const char *sc_name = syscall_get_name(current->last_syscall_num);
      klog_puts(KLOG_CLR_CYAN "LAST SYSCALL CONTEXT:\n" KLOG_CLR_RESET);
      klog_puts("  Syscall:    ");
      klog_puts(sc_name ? sc_name : "unknown");
      klog_puts(" (");
      klog_uint64(current->last_syscall_num);
      klog_puts(") -> returned: ");
      klog_int64(current->last_syscall_ret);
      klog_puts(" (");
      klog_hex64((uint64_t)current->last_syscall_ret);
      klog_puts(")\n");
      klog_puts("  Arguments:  [0]=");
      klog_hex64(current->last_syscall_args[0]);
      klog_puts(", [1]=");
      klog_hex64(current->last_syscall_args[1]);
      klog_puts(", [2]=");
      klog_hex64(current->last_syscall_args[2]);
      klog_puts(", [3]=");
      klog_hex64(current->last_syscall_args[3]);
      klog_puts("\n              [4]=");
      klog_hex64(current->last_syscall_args[4]);
      klog_puts(", [5]=");
      klog_hex64(current->last_syscall_args[5]);
      klog_puts("\n");

      // Active file descriptors / sockets
      if (current->files) {
        klog_puts(KLOG_CLR_CYAN "OPEN DESCRIPTORS (FDS):\n" KLOG_CLR_RESET);
        int fd_count = 0;
        for (int fd = 0; fd < MAX_FDS && fd_count < 16; fd++) {
          vfs_node_t *node = current->files->fds[fd];
          if (node) {
            fd_count++;
            klog_puts("  fd["); klog_uint64(fd); klog_puts("]: ");
            if (current->files->fd_paths[fd] && current->files->fd_paths[fd]->value[0]) {
              klog_puts(current->files->fd_paths[fd]->value);
            } else if (node->name[0]) {
              klog_puts(node->name);
            } else if ((node->flags & FS_TYPE_MASK) == FS_SOCKET) {
              socket_t *s = (socket_t *)node->device;
              if (s) {
                if (s->domain == 1) { // AF_UNIX
                  klog_puts("socket:[unix");
                  if (s->state == SS_CONNECTED) klog_puts(",connected");
                  else if (s->state == SS_LISTENING) klog_puts(",listening");
                  else if (s->state == SS_CONNECTING) klog_puts(",connecting");
                  else if (s->state == SS_DISCONNECTING) klog_puts(",disconnecting");
                  else if (s->state == SS_UNCONNECTED) klog_puts(",unconnected");
                  klog_puts("]");
                } else if (s->domain == 2) {
                  klog_puts("socket:[inet]");
                } else if (s->domain == 16) {
                  klog_puts("socket:[netlink]");
                } else {
                  klog_puts("socket");
                }
              } else {
                klog_puts("socket");
              }
            } else if ((node->flags & FS_TYPE_MASK) == FS_PIPE) {
              klog_puts("pipe");
            } else if ((node->flags & FS_TYPE_MASK) == FS_CHARDEV) {
              klog_puts("chardev");
            } else {
              klog_puts("<vfs_node>");
            }
            klog_puts(" (flags="); klog_hex64(current->files->fd_flags[fd]); klog_puts(")\n");
          }
        }
        if (fd_count == 0) {
          klog_puts("  <none>\n");
        }
      }

      klog_rflags_decoded(regs->rflags);

      // Print all General Purpose Registers
      klog_puts("REGISTERS:\n");
      klog_puts("  RAX="); klog_hex64(regs->rax); klog_puts(" RBX="); klog_hex64(regs->rbx);
      klog_puts(" RCX="); klog_hex64(regs->rcx); klog_puts(" RDX="); klog_hex64(regs->rdx);
      klog_puts("\n  RSI="); klog_hex64(regs->rsi); klog_puts(" RDI="); klog_hex64(regs->rdi);
      klog_puts(" RBP="); klog_hex64(regs->rbp); klog_puts(" RSP="); klog_hex64(regs->rsp);
      klog_puts("\n  R8 ="); klog_hex64(regs->r8);  klog_puts(" R9 ="); klog_hex64(regs->r9);
      klog_puts(" R10="); klog_hex64(regs->r10); klog_puts(" R11="); klog_hex64(regs->r11);
      klog_puts("\n  R12="); klog_hex64(regs->r12); klog_puts(" R13="); klog_hex64(regs->r13);
      klog_puts(" R14="); klog_hex64(regs->r14); klog_puts(" R15="); klog_hex64(regs->r15);
      klog_puts("\n  FS_BASE="); klog_hex64(current->fs_base);
      klog_puts(" GS_BASE="); klog_hex64(current->gs_base);
      if (current->mm) {
          klog_puts(" BRK="); klog_hex64(current->mm->brk_current);
      }
      klog_puts("\n  CR3="); klog_hex64((uint64_t)vmm_get_active_pml4());
      klog_puts("\n");

      // Pointer-like register inspection
      klog_puts("REGISTER MEMORY INSPECTION:\n");
      klog_dump_ptr("RAX", regs->rax);
      klog_dump_ptr("RBX", regs->rbx);
      klog_dump_ptr("RCX", regs->rcx);
      klog_dump_ptr("RDX", regs->rdx);
      klog_dump_ptr("RSI", regs->rsi);
      klog_dump_ptr("RDI", regs->rdi);
      klog_dump_ptr("RBP", regs->rbp);
      klog_dump_ptr("R8 ", regs->r8);
      klog_dump_ptr("R9 ", regs->r9);
      klog_dump_ptr("R10", regs->r10);
      klog_dump_ptr("R11", regs->r11);
      klog_dump_ptr("R12", regs->r12);
      klog_dump_ptr("R13", regs->r13);
      klog_dump_ptr("R14", regs->r14);
      klog_dump_ptr("R15", regs->r15);

      // Hex dump of code at RIP
      uint64_t *pml4 = vmm_get_active_pml4();
      klog_puts("CODE AT RIP: ");
      for (int i = -8; i < 24; i++) {
          uint64_t vaddr = regs->rip + i;
          uint64_t phys = vmm_virt_to_phys(pml4, vaddr);
          if (phys) {
              uint8_t b = *(uint8_t*)(phys + pmm_get_hhdm_offset());
              if (i == 0) klog_puts(KLOG_CLR_GREEN ">");
              const char *_h = "0123456789ABCDEF";
              klog_putchar(_h[(b >> 4) & 0xF]);
              klog_putchar(_h[b & 0xF]);
              if (i == 0) klog_puts("<" KLOG_CLR_RESET);
              klog_putchar(' ');
          } else {
              klog_puts("?? ");
          }
      }
      klog_puts("\n");

      // Stack Snapshot
      klog_puts("USER STACK (RSP):\n");
      for (int i = 0; i < 16; i++) {
          uint64_t saddr = regs->rsp + (i * 8);
          uint64_t phys = vmm_virt_to_phys(pml4, saddr);
          klog_puts("  ["); klog_hex64(saddr); klog_puts("] = ");
          if (phys != 0) {
              uint64_t val = *(uint64_t*)(phys + pmm_get_hhdm_offset());
              klog_hex64(val);
              // Try to find if it corresponds to any VMA or is a string
              struct vma *sv = vma_find(&current->mm->vmas, val);
              if (sv) {
                  klog_puts(" (VMA: "); klog_hex64(sv->start); klog_puts(")");
              }
          } else {
              klog_puts("<unmapped>");
          }
          klog_puts("\n");
      }
      
      // Simple Userland Backtrace (RBP-based)
      klog_puts("USER BACKTRACE (RBP):\n");
      uint64_t curr_rbp = regs->rbp;
      for (int i = 0; i < 16; i++) {
          if (curr_rbp < 0x1000 || curr_rbp >= 0x0000800000000000ULL) break;
          uint64_t phys_rbp = vmm_virt_to_phys(pml4, curr_rbp);
          if (!phys_rbp) break;
          
          uint64_t *rbp_ptr = (uint64_t*)(phys_rbp + pmm_get_hhdm_offset());
          // [0] = old RBP, [1] = return address
          uint64_t next_rbp = rbp_ptr[0];
          uint64_t ret_addr = rbp_ptr[1];
          
          klog_puts("  #"); klog_uint64(i); klog_puts(": "); klog_hex64(ret_addr);
          struct vma *rv = vma_find(&current->mm->vmas, ret_addr);
          if (rv) {
              klog_puts(" (VMA: "); klog_hex64(rv->start); klog_puts(")");
          }
          klog_puts("\n");
          
          if (next_rbp <= curr_rbp) break; // Avoid infinite loops
          curr_rbp = next_rbp;
      }

      // Process context extra info
      klog_puts("\nPROCESS EXTRA INFO:\n");
      klog_puts("  CWD: "); klog_puts(current->cwd_path); klog_puts("\n");
      klog_puts("  UID/GID: "); klog_uint64(current->uid); klog_puts("/"); klog_uint64(current->gid);
      klog_puts("  Pending Signals: "); klog_hex64(current->pending_signals);
      klog_puts("  Signal Mask: "); klog_hex64(current->signal_mask);
      klog_puts("\n");

      // Virtual Memory Area (VMA) Dump
      klog_puts("PROCESS VMAs:\n");
      vma_dump(&current->mm->vmas);
      klog_puts(KLOG_CLR_RED "################################################################################" KLOG_CLR_RESET "\n");
    }

    // 2. Add to /dev/faults for userland monitors
    fault_log_add(regs, sig, addr);

    // 3. Mark signal for delivery and record fault address
    current->fault_addr = addr;
    current->fault_code = (uint32_t)regs->err_code;
    current->pending_signals |= (1ULL << (sig - 1));
  } else {
    isr_panic(regs, "User fault with no thread context");
  }
}

static void page_fault_handler(struct registers *regs) {
  uint64_t cr2;
  __asm__ volatile("mov %%cr2, %0" : "=r"(cr2));

  struct thread *current = sched_get_current();
  if (current) {
    current->fault_addr = cr2;
    current->fault_code = (uint32_t)regs->err_code;
  }

  if (vmm_handle_page_fault(cr2, regs->err_code, regs) != 0) {
    if ((regs->cs & 0x3) == 0x3) {
      isr_report_user_fault(regs, SIGSEGV, cr2);
    } else {
      if (extable_fixup(regs)) {
        return;
      }
      klog_puts("[VMM] KERNEL-mode fault could not be handled by paging "
                "engine!\n");
      /* Everything the paging engine knew, written straight to the serial port
       * without a lock: isr_panic() only prints to the console, and the box is
       * about to stop listening to anything else. */
      kpf_dump_page_fault(regs, cr2);
      isr_panic(regs, "Unhandled Kernel Page Fault");
    }
  }
}

static void gpf_handler(struct registers *regs) {
  if ((regs->cs & 0x3) == 0x3) {
    isr_report_user_fault(regs, SIGSEGV, regs->rip);
  } else {
    isr_panic(regs, "Unhandled General Protection Fault");
  }
}

static void invalid_opcode_handler(struct registers *regs) {
  if ((regs->cs & 0x3) == 0x3) {
    isr_report_user_fault(regs, SIGILL, regs->rip);
  } else {
    isr_panic(regs, "Unhandled Invalid Opcode");
  }
}

static void stack_fault_handler(struct registers *regs) {
  if ((regs->cs & 0x3) == 0x3) {
    isr_report_user_fault(regs, SIGSTKFLT, regs->rsp);
  } else {
    isr_panic(regs, "Unhandled Stack Fault");
  }
}

/*
 * int3 from user mode.  This is how debuggers plant breakpoints and how
 * GLib's G_BREAKPOINT() (g_error, g_assert) deliberately traps; the only
 * correct disposition is SIGTRAP.  The CPU saved the address *after* the
 * int3, so a handler that returns simply resumes there; Linux reports the
 * trap address itself (rip - 1) as si_addr.
 */
static void breakpoint_handler(struct registers *regs) {
  if ((regs->cs & 0x3) == 0x3) {
    isr_report_user_fault(regs, SIGTRAP, regs->rip - 1);
  } else {
    isr_panic(regs, "Unhandled Breakpoint Exception");
  }
}

/* into (overflow) from user mode; Linux reports it as SIGSEGV. */
static void overflow_handler(struct registers *regs) {
  if ((regs->cs & 0x3) == 0x3) {
    isr_report_user_fault(regs, SIGSEGV, regs->rip);
  } else {
    isr_panic(regs, "Unhandled Overflow Exception");
  }
}

void isr_init_exceptions(void) {
  register_interrupt_handler(3, breakpoint_handler);
  register_interrupt_handler(4, overflow_handler);
  register_interrupt_handler(6, invalid_opcode_handler);
  register_interrupt_handler(12, stack_fault_handler);
  register_interrupt_handler(13, gpf_handler);
  register_interrupt_handler(14, page_fault_handler);
  fpu_init(); // Register #NM (vector 7) handler for Lazy FPU switching
}


void isr_handler(struct registers *regs) {
  if (interrupt_handlers[regs->int_no] != 0) {
    isr_t handler = interrupt_handlers[regs->int_no];
    handler(regs);

    if ((regs->cs & 0x3) == 0x3) {
      /* Signal frame construction writes the user stack directly.  The CPU
       * clears RFLAGS.AC on exception entry, so open a window here (no-op
       * without SMAP). */
      user_access_begin();
      signal_deliver(regs);
      user_access_end();
    }

    send_eoi(regs);

    /* Only hardware IRQs (and IPIs) are preemption points here, and only
     * after EOI: an exception return may be an extable fixup, and switching
     * with a vector still in-service starves this CPU's LAPIC. */
    if (regs->int_no >= 32)
      sched_check_resched((regs->cs & 0x3) == 0x3);
    return;
  }

  if (regs->int_no >= 32) {
    if ((regs->cs & 0x3) == 0x3) {
      user_access_begin();
      signal_deliver(regs);
      user_access_end();
    }
    send_eoi(regs);
    sched_check_resched((regs->cs & 0x3) == 0x3);
    return;
  }

  isr_panic(regs, "Unhandled CPU Exception");
}