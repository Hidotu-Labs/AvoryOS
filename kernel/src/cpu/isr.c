#include "isr.h"
#include "../console/klog.h"
#include "../mm/pmm.h"
#include "../mm/vma.h"
#include "../mm/vmm.h"
#include "../sched/sched.h"
#include "../socket/socket.h"
#include "../syscalls/syscall.h"
#include "apic/lapic.h"
#include "../apic/lapic_timer.h"
#include "arch/x86_64/extable.h"
#include "bug_table.h"
#include "fault.h"
#include "features.h"
#include "fpu.h"
#include "kpf_dump.h"
#include "panic_screen.h"
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
    /* x2APIC EOI is an MSR write and does not need the LAPIC HHDM mapping.
     * In xAPIC mode, however, a process PML4 can lack the MMIO slot even
     * though the permanent kernel PML4 maps it.  Switch only around the EOI
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

// Exception Handling & Signals

/* Only one CPU may own the fatal report.  In SMP crashes, concurrent CPUs
 * used to print separate register/VMA dumps into the same UART stream. */
static volatile uint64_t fatal_report_owner;

static bool fatal_report_claim(void) {
  uint64_t me = (uint64_t)panic_screen_current_apic_id() + 1;
  uint64_t expected = 0;
  if (__atomic_compare_exchange_n(&fatal_report_owner, &expected, me, false,
                                  __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
    return true;
  return expected == me;
}

static void fatal_report_halt(void) {
  for (;;) {
    __asm__ volatile("cli; hlt");
  }
}

static void isr_panic(struct registers *regs, const char *msg);

/* NMI IPI used to freeze every peer before panic diagnostics touch shared
 * serial or display state. Normal NMIs retain the existing fatal-exception
 * behavior. */
static void panic_stop_nmi_handler(struct registers *regs) {
  if (panic_screen_is_active()) {
    panic_screen_ack_stopped_cpu();
    panic_screen_stop_this_cpu();
  }
  isr_panic(regs, "Unexpected Non Maskable Interrupt");
}

static void isr_panic(struct registers *regs, const char *msg) {
  if (!fatal_report_claim())
    fatal_report_halt();

  /* Set this before writing anything: a fault while printing must not start a
   * second full panic path on the same CPU. */
  static volatile int panicking;
  if (__atomic_exchange_n(&panicking, 1, __ATOMIC_ACQ_REL)) {
    kpf_dump_panic_recursion(msg, regs);
    fatal_report_halt();
  }

  if (!panic_screen_stop_other_cpus()) {
    kpf_dump_panic_recursion("fatal exception on another CPU", regs);
    fatal_report_halt();
  }

  /* Serial diagnostics stay independent of all screen and scheduler locks. */
  kpf_dump_panic_entry(msg, regs);

  /* #PF gets its page walk here, after peer CPUs have stopped. Every other
   * fatal exception gets the general register, stack and instruction report;
   * #DF stays on its dedicated minimal-risk path below. */
  if (regs->int_no == 14) {
    uint64_t cr2;
    __asm__ volatile("mov %%cr2, %0" : "=r"(cr2));
    kpf_dump_page_fault(regs, cr2);
  } else if (regs->int_no != 8) {
    kpf_dump_exception(msg, regs);
  }

  uint64_t cr2 = 0;
  bool has_cr2 = regs->int_no == 14 || regs->int_no == 8;
  if (has_cr2)
    __asm__ volatile("mov %%cr2, %0" : "=r"(cr2));
  panic_screen_render(msg, regs, has_cr2, cr2);
  serial_flush_sync();
  fatal_report_halt();
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

/* Translate a user address into a kernel-readable HHDM pointer for the fault
 * report.  The direct map does not necessarily cover every frame a user PTE
 * can name (device memory, stale entries, boot-time holes), and this code runs
 * inside the exception path: dereferencing phys + hhdm unvalidated turns a
 * user fault report into a nested #PF and a triple-fault reset.  Validating
 * the direct-map entry first keeps the report alive. */
static bool klog_user_kaddr(uint64_t *pml4, uint64_t uaddr, uint64_t *kaddr_out) {
    uint64_t phys;

    if (!pml4)
        return false;
    phys = vmm_virt_to_phys(pml4, uaddr);
    if (!phys)
        return false;
    if (!vmm_virt_to_phys(pml4, phys + pmm_get_hhdm_offset()))
        return false;
    *kaddr_out = phys + pmm_get_hhdm_offset();
    return true;
}

/* Resolve runtime addresses to the mapped image and file offset without doing
 * filesystem I/O or faulting in pages from the exception path. */
static void klog_resolve_fault_address(struct thread *current, uint64_t addr) {
    if (addr >= 0xFFFFFFFF80000000ULL) {
        klog_puts(" [kernel-image+");
        klog_hex64(addr - 0xFFFFFFFF80000000ULL);
        klog_puts("]");
        return;
    }
    if (addr >= 0x0000800000000000ULL) {
        klog_puts(" [non-canonical]");
        return;
    }
    if (!current || !current->mm)
        return;

    struct vma *v = vma_find(&current->mm->vmas, addr);
    if (!v) {
        klog_puts(" [no VMA]");
        return;
    }
    klog_puts(" [");
    if (v->file_node) {
        vfs_node_t *node = (vfs_node_t *)v->file_node;
        klog_puts(node->name[0] ? node->name : "file");
        klog_puts("+file-offset=");
        klog_hex64(v->offset + (addr - v->start));
    } else {
        klog_puts("anonymous+");
        klog_hex64(addr - v->start);
    }
    klog_puts(", vma="); klog_hex64(v->start); klog_puts("-");
    klog_hex64(v->end); klog_puts(", prot="); klog_hex64(v->prot);
    klog_puts(", flags="); klog_hex64(v->flags); klog_puts("]");
}

static void klog_page_fault_bits(uint64_t err) {
    klog_puts("  Page-fault bits: ");
    klog_puts((err & 1) ? "P=protection " : "P=not-present ");
    klog_puts((err & 2) ? "W=write " : "W=read ");
    klog_puts((err & 4) ? "U=user " : "U=supervisor ");
    if (err & 8) klog_puts("RSVD=1 ");
    if (err & 16) klog_puts("I=instruction-fetch ");
    if (err & 32) klog_puts("PK=1 ");
    if (err & 64) klog_puts("SS=1 ");
    if (err & 128) klog_puts("SGX=1 ");
    klog_puts("\n");
}

static void klog_dump_ptr(struct thread *current, const char *reg_name,
                          uint64_t val) {
    if (val < 0x10000) return; // likely small constant or null
    if (val >= 0x0000800000000000ULL && val < 0xFFFF800000000000ULL) {
        klog_puts("  *"); klog_puts(reg_name); klog_puts(" (");
        klog_hex64(val); klog_puts("): <non-canonical pointer>\n");
        return;
    }
    if (val >= 0xFFFF800000000000ULL) {
        klog_puts("  *"); klog_puts(reg_name); klog_puts(" (");
        klog_hex64(val); klog_puts("): ");
        klog_resolve_fault_address(current, val);
        klog_puts(" <not dereferenced>\n");
        return;
    }
    
    uint64_t *pml4 = vmm_get_active_pml4();
    uint64_t hhdm_addr;
    if (klog_user_kaddr(pml4, val, &hhdm_addr)) {
        klog_puts("  *"); klog_puts(reg_name); klog_puts(" ("); klog_hex64(val); klog_puts("): ");
        klog_resolve_fault_address(current, val);
        klog_puts(" ");
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
    } else {
        klog_puts("  *"); klog_puts(reg_name); klog_puts(" ("); klog_hex64(val);
        klog_puts("): <user address not backed by a readable frame>\n");
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

static const char *thread_state_name(thread_state_t state) {
  switch (state) {
    case THREAD_RUNNING: return "RUNNING";
    case THREAD_READY:   return "READY";
    case THREAD_BLOCKED: return "BLOCKED";
    case THREAD_SLEEPING: return "SLEEPING";
    case THREAD_DEAD:    return "DEAD";
    case THREAD_ZOMBIE:  return "ZOMBIE";
    default:             return "?";
  }
}

static volatile uint32_t user_fault_detail_count;

/* Serializes the multi-line fault dumps. Concurrent faults from several
 * threads (llvmpipe workers, JVM GC threads) otherwise interleave klog
 * fragments into unreadable soup. try_acquire only: kfence-style, never
 * block in the fault path — losers skip logging but still queue delivery
 * and record /dev/faults. */
/* This is a try-only diagnostic gate, never a blocking raw lock. Preserve
 * the caller's interrupt state: masking IRQs across the entire dump delays
 * TLB acknowledgements for hundreds of milliseconds during a userspace
 * abort. A preempted reporter is safe because contenders simply skip it. */
static rawspinlock_t user_fault_log_lock = RAWSPINLOCK_INIT;

/* ── Fault-address VMA neighborhood diagnostics ──────────────────────────
 * A bare "[no VMA]" on CR2/RSP cannot tell a genuine stack overflow from a
 * truncated mapping, so snapshot the surrounding VMAs while holding mm->lock
 * and print after releasing it (klog can take console locks). */
static void isr_print_vma_snapshot(const char *label,
                                   const struct vma_snapshot *s) {
  klog_puts("  ");
  klog_puts(label);
  klog_puts(": ");
  if (!s || !s->valid) {
    klog_puts("<none>\n");
    return;
  }
  klog_hex64(s->start);
  klog_puts(" - ");
  klog_hex64(s->end);
  klog_puts(" (");
  klog_uint64(s->end - s->start);
  klog_puts(" bytes, ");
  klog_puts((s->prot & 0x1) ? "r" : "-");
  klog_puts((s->prot & 0x2) ? "w" : "-");
  klog_puts((s->prot & 0x4) ? "x" : "-");
  klog_puts(",");
  if (s->flags & 0x01)
    klog_puts("shared,");
  if (s->flags & 0x02)
    klog_puts("private,");
  if (s->flags & 0x20)
    klog_puts("anon,");
  if (s->flags & MAP_GROWSDOWN)
    klog_puts("GROWSDOWN,");
  if (s->flags & MAP_STACK)
    klog_puts("STACK,");
  if (s->flags & 0x100000000ULL)
    klog_puts("SYSV_SHM,");
  if (s->flags & 0x200000000ULL)
    klog_puts("HUGEPAGE,");
  if (s->flags & 0x400000000ULL)
    klog_puts("PAGECACHE,");
  klog_puts("flags=");
  klog_hex64(s->flags);
  if (s->fd != -1) {
    klog_puts(" fd=");
    klog_uint64((uint64_t)s->fd);
    klog_puts(" off=");
    klog_hex64(s->offset);
  }
  klog_puts(")\n");
}

static void isr_report_vma_neighborhood(struct thread *current, uint64_t addr,
                                        uint64_t rsp) {
  struct vma_snapshot fault_prev, fault_hit, fault_next;
  struct vma_snapshot rsp_prev, rsp_hit, rsp_next;
  struct vma_snapshot grow_hit;
#define ISR_STACK_SURVEY_CAP 32
  struct vma_snapshot stacks[ISR_STACK_SURVEY_CAP];
  bool stack_guarded[ISR_STACK_SURVEY_CAP];
  uint64_t stack_guard_size[ISR_STACK_SURVEY_CAP];
  int stack_total = 0;
  int stack_shown = 0;
  bool have_grow = false;
  uint64_t grow_start = 0;
  bool locked = false;

  fault_prev.valid = fault_hit.valid = fault_next.valid = false;
  rsp_prev.valid = rsp_hit.valid = rsp_next.valid = false;
  grow_hit.valid = false;

  if (!current->mm)
    goto print;
  /* Never block in the fault path: the fault may have interrupted code that
   * already holds mm->lock (e.g. a nested fault during copy_to_user). */
  if (!spinlock_try_acquire(&current->mm->lock))
    goto print_locked_busy;
  locked = true;
  vma_snapshot_neighbors(&current->mm->vmas, addr, &fault_prev, &fault_hit,
                         &fault_next);
  if (rsp != addr)
    vma_snapshot_neighbors(&current->mm->vmas, rsp, &rsp_prev, &rsp_hit,
                           &rsp_next);
  else {
    rsp_prev = fault_prev;
    rsp_hit = fault_hit;
    rsp_next = fault_next;
  }
  /* Stack-growth eligibility uses the same 8MB window as the fault handler. */
  {
    struct vma *grow =
        vma_find_growdown(&current->mm->vmas, addr, 8 * 1024 * 1024);
    if (grow) {
      have_grow = true;
      grow_start = grow->start;
      grow_hit.start = grow->start;
      grow_hit.end = grow->end;
      grow_hit.prot = grow->prot;
      grow_hit.flags = grow->flags;
      grow_hit.offset = grow->offset;
      grow_hit.fd = grow->fd;
      grow_hit.valid = true;
    }
  }
  /* Survey every pthread stack: size outliers and missing guards explain a
   * marginal overflow (this fault missed by 568B) vs a wild RSP. */
  stack_total = vma_snapshot_stacks(&current->mm->vmas, stacks,
                                    ISR_STACK_SURVEY_CAP);
  stack_shown = stack_total < ISR_STACK_SURVEY_CAP ? stack_total
                                                   : ISR_STACK_SURVEY_CAP;
  for (int i = 0; i < stack_shown; i++) {
    stack_guarded[i] = false;
    stack_guard_size[i] = 0;
    if (!stacks[i].valid || stacks[i].start == 0)
      continue;
    struct vma *g = vma_find(&current->mm->vmas, stacks[i].start - 1);
    if (g && g->prot == 0 && g->end == stacks[i].start) {
      stack_guarded[i] = true;
      stack_guard_size[i] = g->end - g->start;
    }
  }
  spinlock_release(&current->mm->lock);
  locked = false;

print:
  klog_puts(KLOG_CLR_CYAN "VMA NEIGHBORHOOD (fault addr):\n" KLOG_CLR_RESET);
  if (!current->mm) {
    klog_puts("  <no mm>\n");
  } else if (!locked && !fault_prev.valid && !fault_hit.valid &&
             !fault_next.valid && !have_grow) {
    /* Fall through to the per-snapshot prints so the <none> lines still show
     * the address that was probed. */
  }
  isr_print_vma_snapshot("fault-hit ", &fault_hit);
  isr_print_vma_snapshot("fault-prev", &fault_prev);
  isr_print_vma_snapshot("fault-next", &fault_next);
  if (fault_prev.valid && addr >= fault_prev.end) {
    klog_puts("  fault is ");
    klog_uint64(addr - fault_prev.end);
    klog_puts(" bytes above fault-prev end\n");
  }
  if (fault_next.valid && addr < fault_next.start) {
    klog_puts("  fault is ");
    klog_uint64(fault_next.start - addr);
    klog_puts(" bytes below fault-next start\n");
  }
  if (have_grow) {
    klog_puts("  growdown candidate within 8MB at ");
    klog_hex64(grow_start);
    klog_puts("\n");
  } else {
    klog_puts("  no GROWSDOWN VMA within 8MB below fault\n");
  }
  if (rsp != addr) {
    klog_puts(KLOG_CLR_CYAN "VMA NEIGHBORHOOD (RSP):\n" KLOG_CLR_RESET);
    klog_puts("  RSP=");
    klog_hex64(rsp);
    klog_puts(rsp == addr - 8 || rsp == addr + 8 ? " (adjacent to fault: likely push/call)\n" : "\n");
    isr_print_vma_snapshot("rsp-hit ", &rsp_hit);
    isr_print_vma_snapshot("rsp-prev", &rsp_prev);
    isr_print_vma_snapshot("rsp-next", &rsp_next);
    if (rsp_prev.valid && rsp >= rsp_prev.end) {
      klog_puts("  RSP is ");
      klog_uint64(rsp - rsp_prev.end);
      klog_puts(" bytes above rsp-prev end\n");
    }
    if (rsp_next.valid && rsp < rsp_next.start) {
      klog_puts("  RSP is ");
      klog_uint64(rsp_next.start - rsp);
      klog_puts(" bytes below rsp-next start\n");
    }
    if (addr + 8 == rsp || rsp + 8 == addr) {
      klog_puts("  fault == RSP-8: matches a push/call stack-store fault\n");
    }
  }
  klog_puts(KLOG_CLR_CYAN "THREAD STACKS (MAP_STACK):\n" KLOG_CLR_RESET);
  if (!current->mm) {
    klog_puts("  <no mm>\n");
  } else {
    klog_puts("  count=");
    klog_uint64((uint64_t)stack_total);
    klog_puts("\n");
    for (int i = 0; i < stack_shown; i++) {
      if (!stacks[i].valid)
        continue;
      klog_puts("  [");
      klog_uint64((uint64_t)i);
      klog_puts("] ");
      klog_hex64(stacks[i].start);
      klog_puts(" - ");
      klog_hex64(stacks[i].end);
      klog_puts(" (");
      klog_uint64(stacks[i].end - stacks[i].start);
      klog_puts(" bytes) guard=");
      if (stack_guarded[i]) {
        klog_uint64(stack_guard_size[i]);
        klog_puts("B");
      } else {
        klog_puts("NONE");
      }
      if (fault_next.valid && stacks[i].start == fault_next.start &&
          fault_next.valid)
        klog_puts(" <-- fault-next (this thread)");
      klog_puts("\n");
    }
    if (stack_total > stack_shown) {
      klog_puts("  ... truncated, ");
      klog_uint64((uint64_t)(stack_total - stack_shown));
      klog_puts(" more\n");
    }
  }
  return;

print_locked_busy:
  klog_puts(KLOG_CLR_CYAN "VMA NEIGHBORHOOD:\n" KLOG_CLR_RESET
            "  <mm->lock busy; skipped>\n");
}

void isr_report_user_fault(struct registers *regs, int sig,
                           uint64_t addr) {
  struct thread *current = sched_get_current();
  if (current) {
    /* Serialize multi-line dumps; losers still capture/queue/record. */
    bool have_fault_log = rawspinlock_try_acquire(&user_fault_log_lock);
    /* A blocked synchronous SIGSEGV may only be delivered at a later syscall
     * boundary. Keep the exception frame so that deferred delivery and crash
     * reports describe the actual faulting instruction, not that syscall. */
    bool captured_sync_fault = false;
    bool is_sync = (sig == SIGSEGV || sig == SIGBUS || sig == SIGILL || sig == SIGFPE);
    if (is_sync &&
        (regs->int_no == 13 || regs->int_no == 14 || regs->int_no == 6 ||
         regs->int_no == 0 || regs->int_no == 16) &&
        !current->fault_regs_valid) {
      current->fault_regs = *regs;
      current->fault_regs_valid = true;
      captured_sync_fault = true;
    }

    /* Synchronous faults cannot make progress while blocked: returning to the
     * faulting RIP just re-faults immediately. Linux force-kills in this
     * case. Without this, a SIGSEGV inside a SIGSEGV handler (e.g. KDE's
     * libKF6Crash handler faulting on null at +0x51C1 while SIGSEGV is
     * blocked via SA_NODEFER==0) re-queues forever and spams:
     *   [SIGNAL] Exception 11 ... comm='systemsettings' ... addr=0x0
     * The same applies to a second sync fault before sigreturn (recursive
     * handler crash): fault_regs_valid is still set from the first fault. */
    if (is_sync && sig >= 1 && sig <= 64) {
      uint64_t sig_bit = (1ULL << (sig - 1));
      bool blocked = (current->signal_mask & sig_bit) != 0;
      /* fault_regs_valid && !captured means a previous sync fault hasn't
       * sigreturned yet. That's only a recursive handler crash if the new
       * fault is at a *different* location: signal_deliver()'s DFL path
       * re-calls isr_report_user_fault() with the same regs for the detailed
       * dump, which must not be mistaken for recursion. */
      bool same_fault = current->fault_regs_valid &&
                        regs->rip == current->fault_regs.rip &&
                        addr == current->fault_addr;
      bool recursive = current->fault_regs_valid && !captured_sync_fault &&
                       !same_fault;
      if (blocked || recursive) {
        if (have_fault_log) {
          klog_puts("[SIGNAL] Sync fault ");
          klog_uint64((uint64_t)sig);
          klog_puts(" (");
          klog_puts(get_signal_name(sig));
          klog_puts(blocked ? ") while blocked" : ") recursive in handler");
          klog_puts(" comm='");
          klog_puts(current->comm);
          klog_puts("' tid=");
          klog_uint64(current->tid);
          klog_puts(" rip=");
          klog_hex64(regs->rip);
          klog_resolve_fault_address(current, regs->rip);
          klog_puts(" addr=");
          klog_hex64(addr);
          if (recursive && current->fault_regs_valid) {
            klog_puts(" first_rip=");
            klog_hex64(current->fault_regs.rip);
            klog_resolve_fault_address(current, current->fault_regs.rip);
          }
          klog_puts("; terminating (handler would loop)\n");
        }
        fault_log_add(regs, sig, addr);
        current->fault_addr = addr;
        current->fault_code = (uint32_t)regs->err_code;
        current->exit_status = 128 + sig;
        if (have_fault_log)
          rawspinlock_release(&user_fault_log_lock);
        sched_terminate_thread_group(current);
        process_do_exit(128 + sig);
      }
    }

    bool has_custom_handler = false;
    if (sig >= 1 && sig <= 64) {
      void *handler = (void *)current->signal_handlers[sig - 1].sa_handler;
      if (handler != NULL && handler != (void *)1) {
        has_custom_handler = true;
      }
    }

    if (has_custom_handler) {
      if (is_sync && KLOG_VERBOSE) {
        /* Rate-limit: the handler-crash loop used to spam this line
         * thousands of times (same tid/rip/addr). Dedup identical faults
         * and sample the rest. last_report_* is shared with the DFL path
         * so a custom->DFL transition doesn't reset the suppression. */
        if (current->last_report_sig == sig &&
            current->last_report_rip == regs->rip &&
            current->last_report_addr == addr) {
          /* Still queue for delivery below; just don't log again. */
        } else {
          current->last_report_sig = sig;
          current->last_report_rip = regs->rip;
          current->last_report_addr = addr;
          uint32_t seen = __atomic_add_fetch(&user_fault_detail_count, 1,
                                             __ATOMIC_RELAXED);
          if (!have_fault_log) {
            /* Another CPU/thread is dumping; delivery + /dev/faults below
             * still happen, we just don't interleave klog fragments. */
          } else if (seen <= 8 || (seen & 0xFFu) == 0) {
            klog_puts("[SIGNAL] Exception ");
            klog_uint64(sig);
            klog_puts(" (");
            klog_puts(get_signal_name(sig));
            klog_puts(") to custom handler comm='");
            klog_puts(current->comm);
            klog_puts("' tid=");
            klog_uint64(current->tid);
            klog_puts(" rip=");
            klog_hex64(regs->rip);
            klog_resolve_fault_address(current, regs->rip);
            klog_puts(" addr=");
            klog_hex64(addr);
            klog_puts(" count=");
            klog_uint64(seen);
            klog_puts("\n");
            /* First few custom-handler faults also get a compact
             * register/stack dump: without it a Qt/QML null-deref
             * (e.g. QV4::MemoryManager) is undebuggable from one line. */
            if (seen <= 4) {
              klog_puts("  VECTOR: "); klog_uint64(regs->int_no);
              klog_puts("  ERR: "); klog_hex64(regs->err_code);
              klog_puts("  CS: "); klog_hex64(regs->cs);
              klog_puts("\n");
              if (regs->int_no == 14)
                klog_page_fault_bits(regs->err_code);
              isr_report_vma_neighborhood(current, addr, regs->rsp);
              klog_rflags_decoded(regs->rflags);
              klog_puts("REGISTERS:\n");
              klog_puts("  RAX="); klog_hex64(regs->rax); klog_puts(" RBX="); klog_hex64(regs->rbx);
              klog_puts(" RCX="); klog_hex64(regs->rcx); klog_puts(" RDX="); klog_hex64(regs->rdx);
              klog_puts("\n  RSI="); klog_hex64(regs->rsi); klog_puts(" RDI="); klog_hex64(regs->rdi);
              klog_puts(" RBP="); klog_hex64(regs->rbp); klog_puts(" RSP="); klog_hex64(regs->rsp);
              klog_puts("\n  R8 ="); klog_hex64(regs->r8);  klog_puts(" R9 ="); klog_hex64(regs->r9);
              klog_puts(" R10="); klog_hex64(regs->r10); klog_puts(" R11="); klog_hex64(regs->r11);
              klog_puts("\n  R12="); klog_hex64(regs->r12); klog_puts(" R13="); klog_hex64(regs->r13);
              klog_puts(" R14="); klog_hex64(regs->r14); klog_puts(" R15="); klog_hex64(regs->r15);
              klog_puts("\n");
              uint64_t *pml4 = vmm_get_active_pml4();
              klog_puts("CODE AT RIP: ");
              for (int i = -8; i < 24; i++) {
                uint64_t vaddr = regs->rip + i;
                uint64_t kaddr;
                if (klog_user_kaddr(pml4, vaddr, &kaddr)) {
                  uint8_t b = *(uint8_t*)kaddr;
                  if (i == 0) klog_puts(">");
                  const char *_h = "0123456789ABCDEF";
                  klog_putchar(_h[(b >> 4) & 0xF]);
                  klog_putchar(_h[b & 0xF]);
                  if (i == 0) klog_puts("<");
                  klog_putchar(' ');
                } else {
                  klog_puts("?? ");
                }
              }
              klog_puts("\nUSER STACK (RSP):\n");
              /* Deep dump: ply_event_loop_stop_watching_fd buries its return
               * address ~264B above RSP (0xB8 locals + pushes + snprintf
               * temporaries). 8 entries only show logger args (file/line
               * strings like 0x11253/0x130A0), never the caller. */
              for (int i = 0; i < 40; i++) {
                uint64_t saddr = regs->rsp + (i * 8);
                uint64_t kaddr;
                klog_puts("  ["); klog_hex64(saddr); klog_puts("] = ");
                if (klog_user_kaddr(pml4, saddr, &kaddr)) {
                  uint64_t val = *(uint64_t*)kaddr;
                  klog_hex64(val);
                  klog_resolve_fault_address(current, val);
                } else {
                  klog_puts("<unmapped>");
                }
                klog_puts("\n");
              }
              const char *sc_name = syscall_get_name(current->last_syscall_num);
              klog_puts("LAST SYSCALL: ");
              klog_puts(sc_name ? sc_name : "unknown");
              klog_puts(" ("); klog_uint64(current->last_syscall_num); klog_puts(") -> ");
              klog_int64(current->last_syscall_ret); klog_puts("\n");
              klog_puts("  args:");
              for (int i = 0; i < 6; i++) {
                klog_puts(" "); klog_hex64(current->last_syscall_args[i]);
              }
              klog_puts("\n");
              klog_puts("EXE: "); klog_puts(current->exe_path[0] ? current->exe_path : "?"); klog_puts("\n");
            }
          } else if (seen == 9) {
            klog_puts("[SIGNAL] repeated custom-handler faults suppressed; "
                      "sampling every 256th (counter is global)\n");
          }
        }
      }
    } else {
      if (current->last_report_sig == sig &&
          current->last_report_rip == regs->rip &&
          current->last_report_addr == addr) {
        if (have_fault_log)
          rawspinlock_release(&user_fault_log_lock);
        return;
      }
      current->last_report_sig = sig;
      current->last_report_rip = regs->rip;
      current->last_report_addr = addr;
      uint32_t seen = __atomic_add_fetch(&user_fault_detail_count, 1,
                                         __ATOMIC_RELAXED);
      if (seen <= 4 && have_fault_log) {
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
      klog_resolve_fault_address(current, regs->rip);
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
      klog_resolve_fault_address(current, addr);
      klog_puts("  VECTOR: "); klog_uint64(regs->int_no);
      klog_puts("  ERR: "); klog_hex64(regs->err_code);
      klog_puts("  CS: "); klog_hex64(regs->cs);
      klog_puts("\n");
      if (regs->int_no == 14)
        klog_page_fault_bits(regs->err_code);

      /* Neighboring VMAs + RSP gap: distinguishes stack overflow (fault/RSP
       * just below a MAP_STACK/PROT_NONE guard) from a truncated mapping. */
      isr_report_vma_neighborhood(current, addr, regs->rsp);

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
          const char *err_sc = syscall_get_name(current->last_error_syscall_num);
          klog_puts(" in syscall ");
          klog_puts(err_sc ? err_sc : "unknown");
          klog_puts(" (");
          klog_uint64(current->last_error_syscall_num);
          klog_puts(")");
          if (current->last_error_path[0]) {
            klog_puts(" path='");
            klog_puts(current->last_error_path);
            klog_puts("'");
          }
          klog_puts("\n");
        }
        if (current->last_stderr[0]) {
          klog_puts(KLOG_CLR_YELLOW "  Last Stderr: " KLOG_CLR_RESET);
          klog_puts(current->last_stderr);
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
        for (uint32_t fd = 0; fd < current->files->capacity && fd_count < 16; fd++) {
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

      /* Threads in this process: a watchdog/canary fault (WebKit's
       * WatchDogQueue is the classic one) usually means the process is trying
       * to exit while a sibling is still parked somewhere.  Show where every
       * thread is.  Trylock only - the fault handler must never wait on the
       * scheduler's tid_lock. */
      {
        extern spinlock_t tid_lock;
        if (spinlock_try_acquire(&tid_lock)) {
          int shown = 0;
          klog_puts(KLOG_CLR_CYAN "PROCESS THREADS:\n" KLOG_CLR_RESET);
          for (struct thread *t = sched_get_thread_list_head(); t;
               t = t->global_next) {
            if (t->tgid != current->tgid)
              continue;
            if (shown >= 24)
              break;
            shown++;
            klog_puts("  tid=");
            klog_uint64(t->tid);
            klog_puts(" comm='");
            klog_puts(t->comm[0] ? t->comm : "?");
            klog_puts("' state=");
            klog_puts(thread_state_name(t->state));
            if (t->last_syscall_num) {
              const char *sn = syscall_get_name(t->last_syscall_num);
              klog_puts(" syscall=");
              klog_puts(sn ? sn : "?");
              klog_puts("#");
              klog_uint64(t->last_syscall_num);
            }
            if (t->last_kernel_func) {
              klog_puts(" at ");
              klog_puts(t->last_kernel_file ? t->last_kernel_file : "?");
              klog_puts(":");
              klog_uint64(t->last_kernel_line);
              klog_puts(" ");
              klog_puts(t->last_kernel_func);
            }
            if (t->blocked_since_ms) {
              uint64_t now_ms = lapic_timer_get_ms();
              klog_puts(" parked=");
              klog_uint64(now_ms > t->blocked_since_ms
                              ? now_ms - t->blocked_since_ms
                              : 0);
              klog_puts("ms");
            }
            klog_puts("\n");
          }
          if (shown == 0)
            klog_puts("  <none>\n");
          spinlock_release(&tid_lock);
        } else {
          klog_puts(KLOG_CLR_CYAN "PROCESS THREADS:\n" KLOG_CLR_RESET
                    "  <tid_lock busy; skipped>\n");
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
      klog_dump_ptr(current, "RAX", regs->rax);
      klog_dump_ptr(current, "RBX", regs->rbx);
      klog_dump_ptr(current, "RCX", regs->rcx);
      klog_dump_ptr(current, "RDX", regs->rdx);
      klog_dump_ptr(current, "RSI", regs->rsi);
      klog_dump_ptr(current, "RDI", regs->rdi);
      klog_dump_ptr(current, "RBP", regs->rbp);
      klog_dump_ptr(current, "R8 ", regs->r8);
      klog_dump_ptr(current, "R9 ", regs->r9);
      klog_dump_ptr(current, "R10", regs->r10);
      klog_dump_ptr(current, "R11", regs->r11);
      klog_dump_ptr(current, "R12", regs->r12);
      klog_dump_ptr(current, "R13", regs->r13);
      klog_dump_ptr(current, "R14", regs->r14);
      klog_dump_ptr(current, "R15", regs->r15);

      // Hex dump of code at RIP
      uint64_t *pml4 = vmm_get_active_pml4();
      klog_puts("CODE AT RIP: ");
      for (int i = -8; i < 24; i++) {
          uint64_t vaddr = regs->rip + i;
          uint64_t kaddr;
          if (klog_user_kaddr(pml4, vaddr, &kaddr)) {
              uint8_t b = *(uint8_t*)kaddr;
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

      // Stack Snapshot (deep: captures callers buried under large frames)
      klog_puts("USER STACK (RSP):\n");
      for (int i = 0; i < 48; i++) {
          uint64_t saddr = regs->rsp + (i * 8);
          uint64_t kaddr;
          klog_puts("  ["); klog_hex64(saddr); klog_puts("] = ");
          if (klog_user_kaddr(pml4, saddr, &kaddr)) {
              uint64_t val = *(uint64_t*)kaddr;
              klog_hex64(val);
              klog_resolve_fault_address(current, val);
          } else {
              klog_puts("<unmapped>");
          }
          klog_puts("\n");
      }
      
      // Simple Userland Backtrace (RBP-based)
      klog_puts("USER BACKTRACE (RBP):\n");
      uint64_t curr_rbp = regs->rbp;
      unsigned abort_pointer_dumps = 0;
      for (int i = 0; i < 16; i++) {
          if (curr_rbp < 0x1000 || curr_rbp >= 0x0000800000000000ULL) break;
          uint64_t old_rbp_kaddr, ret_addr_kaddr;
          if ((curr_rbp & 7) ||
              !klog_user_kaddr(pml4, curr_rbp, &old_rbp_kaddr) ||
              !klog_user_kaddr(pml4, curr_rbp + sizeof(uint64_t),
                               &ret_addr_kaddr))
              break;
          // [0] = old RBP, [1] = return address
          uint64_t next_rbp = *(uint64_t *)old_rbp_kaddr;
          uint64_t ret_addr = *(uint64_t *)ret_addr_kaddr;
          
          klog_puts("  #"); klog_uint64(i); klog_puts(": "); klog_hex64(ret_addr);
          klog_resolve_fault_address(current, ret_addr);
          klog_puts("\n");

          /* abort() clobbers the allocator's argument registers. Preserve
           * bounded caller locals so a heap-corruption report can identify
           * the rejected chunk and arena, rather than only the abort site.
           * Read existing translations only; diagnostics must not fault in
           * arbitrary userspace memory. */
          if (sig == 6 && i < 6) {
              for (unsigned slot = 1; slot <= 8; slot++) {
                  uint64_t saddr = curr_rbp - slot * sizeof(uint64_t);
                  uint64_t kaddr;
                  if (saddr < regs->rsp ||
                      !klog_user_kaddr(pml4, saddr, &kaddr))
                      continue;
                  uint64_t val = *(uint64_t *)kaddr;
                  klog_puts("    local["); klog_hex64(saddr);
                  klog_puts("] = "); klog_hex64(val);
                  klog_puts("\n");
                  struct vma *pv = current->mm
                      ? vma_find(&current->mm->vmas, val) : NULL;
                  if (abort_pointer_dumps < 8 && pv && !pv->file_node &&
                      (pv->prot & 2) && !(pv->flags & MAP_STACK) &&
                      val >= pv->start + 16 && !(val & 15)) {
                      klog_dump_ptr(current, "caller data", val);
                      klog_dump_ptr(current, "preceding bytes", val - 16);
                      abort_pointer_dumps++;
                  }
              }
          }
          
          if (next_rbp <= curr_rbp) break; // Avoid infinite loops
          curr_rbp = next_rbp;
      }

      /* Stack scan for return addresses: when RBP is corrupt (e.g. 0x40
       * wild watch in ply_event_loop_stop_watching_fd) the RBP chain is
       * empty, but return addresses remain on the stack in executable
       * VMAs. Scan the dumped range and report plausible callers. */
      klog_puts("USER STACK CALLERS (scan):\n");
      for (int i = 0; i < 48; i++) {
          uint64_t saddr = regs->rsp + (i * 8);
          uint64_t kaddr;
          if (!klog_user_kaddr(pml4, saddr, &kaddr))
              continue;
          uint64_t val = *(uint64_t*)kaddr;
          if (val < 0x1000 || val >= 0x0000800000000000ULL)
              continue;
          struct vma *rv = NULL;
          if (current->mm)
              rv = vma_find(&current->mm->vmas, val);
          if (!rv)
              continue;
          /* Only executable mappings can be return addresses; this skips
           * rodata strings like 0x11253/0x130A0 that otherwise flood the
           * scan (they are logger file/func args, not callers). */
          if (!(rv->prot & 0x4))
              continue;
          /* Heuristic: return address must be in executable mapping and
           * 8-byte aligned Stack content; skip the faulting RIP itself. */
          if (val == regs->rip)
              continue;
          klog_puts("  ["); klog_hex64(saddr); klog_puts("] ret=");
          klog_hex64(val);
          klog_resolve_fault_address(current, val);
          klog_puts("\n");
      }

      // Process context extra info
      klog_puts("\nPROCESS EXTRA INFO:\n");
      klog_puts("  EXE: "); klog_puts(current->exe_path[0] ? current->exe_path : "?"); klog_puts("\n");
      klog_puts("  CWD: "); klog_puts(current->cwd_path); klog_puts("\n");
      klog_puts("  UID/GID: "); klog_uint64(current->uid); klog_puts("/"); klog_uint64(current->gid);
      klog_puts("  Pending Signals: "); klog_hex64(current->pending_signals);
      klog_puts("  Signal Mask: "); klog_hex64(current->signal_mask);
      klog_puts("\n");

      klog_puts(KLOG_CLR_RED "################################################################################" KLOG_CLR_RESET "\n");
      } else if (have_fault_log && (seen <= 8 || (seen & 0xFFu) == 0)) {
        klog_puts("[USERFAULT] process='");
        klog_puts(current->comm);
        klog_puts("' tid=");
        klog_uint64(current->tid);
        klog_puts(" signal=");
        klog_uint64(sig);
        klog_puts(" rip=");
        klog_hex64(regs->rip);
        klog_puts(" addr=");
        klog_hex64(addr);
        klog_puts(" err=");
        klog_hex64(regs->err_code);
        klog_puts(" count=");
        klog_uint64(seen);
        klog_puts("\n");
      } else if (have_fault_log && seen == 9) {
        klog_puts("[USERFAULT] repeated detailed reports suppressed; "
                  "sampling every 256th (counter is global)\n");
      }
    }

    // 2. Add to /dev/faults for userland monitors
    fault_log_add(regs, sig, addr);

    // 3. Mark signal for delivery and retain fault metadata. If SIGSEGV is
    // deferred while blocked, keep the address/error paired with the first
    // saved exception frame instead of combining it with a later retry.
    if (!is_sync || captured_sync_fault || !current->fault_regs_valid) {
      current->fault_addr = addr;
      current->fault_code = (uint32_t)regs->err_code;
    }
    current->pending_signals |= (1ULL << (sig - 1));
    if (have_fault_log)
      rawspinlock_release(&user_fault_log_lock);
  } else {
    isr_panic(regs, "User fault with no thread context");
  }
}

static void page_fault_handler(struct registers *regs) {
  uint64_t cr2;
  __asm__ volatile("mov %%cr2, %0" : "=r"(cr2));

  if (vmm_handle_page_fault(cr2, regs->err_code, regs) != 0) {
    if ((regs->cs & 0x3) == 0x3) {
      isr_report_user_fault(regs, SIGSEGV, cr2);
    } else {
      if (extable_fixup(regs)) {
        return;
      }
      /* isr_panic() claims the report, stops peer CPUs, then logs the saved
       * CR2 and page-table walk over the lock-free serial path. */
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
  } else if (asc_bug_handle_invalid_opcode(regs)) {
    /* Imported WARN(): reported and RIP advanced past the ud2. */
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

/* Delivered when a fault occurs while the CPU is delivering another fault
 * (typically a nested #PF inside the page-fault handler).  The gate carries
 * IST1 (see gdt.c/idt.c), so the frame is valid even when the stack that
 * faulted is not; without it these reset the machine silently.  CR2 still
 * holds the second fault's address, and kpf_dump_page_fault() writes straight
 * to the serial line before the console path can fail. */
static void double_fault_handler(struct registers *regs) {
  /* #DF runs on IST1 and can bypass the framebuffer, serial and scheduler
   * locks. Its report stays compact on serial, while the cached scanout gives
   * the user a readable crash screen even when DRM owns the display. */
  if (!panic_screen_stop_other_cpus())
    fatal_report_halt();
  kpf_dump_double_fault(regs);
  uint64_t cr2 = 0;
  __asm__ volatile("mov %%cr2, %0" : "=r"(cr2));
  panic_screen_render("Double Fault", regs, true, cr2);
  fatal_report_halt();
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
  register_interrupt_handler(2, panic_stop_nmi_handler);
  register_interrupt_handler(3, breakpoint_handler);
  register_interrupt_handler(4, overflow_handler);
  register_interrupt_handler(6, invalid_opcode_handler);
  register_interrupt_handler(8, double_fault_handler);
  register_interrupt_handler(12, stack_fault_handler);
  register_interrupt_handler(13, gpf_handler);
  register_interrupt_handler(14, page_fault_handler);
  fpu_init(); // Register #NM (vector 7) handler for Lazy FPU switching
}


static void isr_dispatch(struct registers *regs) {
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

    return;
  }

  if (regs->int_no >= 32) {
    if ((regs->cs & 0x3) == 0x3) {
      user_access_begin();
      signal_deliver(regs);
      user_access_end();
    }
    send_eoi(regs);
    return;
  }

  isr_panic(regs, "Unhandled CPU Exception");
}

void isr_handler(struct registers *regs) {
  if (regs->int_no == 0x80) {
    syscall_compat_dispatch(regs);
    return; /* Software syscall: no controller EOI or hardirq accounting. */
  }
  int is_hw_irq = (regs->int_no >= 32);

  /* Track hardirq context for LinuxKPI's in_interrupt().  CPU exceptions are
   * not interrupt context; only vectors 32+ (IRQs and IPIs) are. */
  extern void linuxkpi_irq_enter(void) __attribute__((weak));
  extern void linuxkpi_irq_exit(void) __attribute__((weak));

  if (is_hw_irq && linuxkpi_irq_enter)
    linuxkpi_irq_enter();

  isr_dispatch(regs);

  if (is_hw_irq && linuxkpi_irq_exit)
    linuxkpi_irq_exit();

  /* Complete both the controller EOI and LinuxKPI hardirq accounting before
   * a reschedule request can switch away from this interrupt frame. */
  if (is_hw_irq)
    sched_check_resched((regs->cs & 0x3) == 0x3);

  /* rseq: an IRQ (e.g. the timer) may have preempted a restartable critical
   * section or migrated this thread. Publish the current CPU and abort the
   * section before returning to ring 3. Signal delivery above already ran;
   * if it redirected RIP to a handler, the abort check harmlessly no-ops,
   * and the post-handler resume is covered by the syscall/sigreturn hooks. */
  if ((regs->cs & 0x3) == 0x3) {
    struct thread *rt = sched_get_current();
    if (rt) {
      rseq_update_cpu_id(rt);
      rseq_maybe_abort(rt, &regs->rip);
    }
  }
}
