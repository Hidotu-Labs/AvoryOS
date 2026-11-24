#include "isr.h"
#include "isr_internal.h"
#include "../mm/vmm.h"
#include "../sched/sched.h"
#include "../syscalls/syscall.h"
#include "../smp/cpu.h"
#include "arch/x86_64/extable.h"
#include "bug_table.h"
#include "fault.h"
#include "features.h"
#include "fpu.h"
#include "kpf_dump.h"
#include "panic_screen.h"
#include "ktrack.h"
#include "msr.h"
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

/* NMI IPI used to freeze every peer before panic diagnostics touch shared
 * serial or display state. Normal NMIs retain the existing fatal-exception
 * behavior. */
static void panic_stop_nmi_handler(struct registers *regs) {
  /* An NMI that kicks a running guest (AVM) is not an error. */
  extern bool avm_nmi_kick_consume(void) __attribute__((weak));
  if (avm_nmi_kick_consume && avm_nmi_kick_consume())
    return;
  if (panic_screen_is_active()) {
    panic_screen_ack_stopped_cpu();
    panic_screen_stop_this_cpu();
  }
  isr_panic(regs, "Unexpected Non Maskable Interrupt");
}

void isr_panic(struct registers *regs, const char *msg) {
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

  /* The timer tick that normally drains klog's serial queue is gone from
   * here on: make every klog write synchronous. */
  serial_set_panic_mode();

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

static void page_fault_handler(struct registers *regs) {
  uint64_t cr2;
  __asm__ volatile("mov %%cr2, %0" : "=r"(cr2));

  if (cr2 >= 0xFFFFFFFFFF600000ULL && cr2 < 0xFFFFFFFFFF601000ULL &&
      vmm_emulate_vsyscall(cr2, regs)) {
    return;
  }

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
  } else if (extable_fixup(regs)) {
    return;
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

/* Hardware single-step and watchpoint traps are #DB. PTRACE_SINGLESTEP sets
 * TF in the tracee's saved RFLAGS; clear it here so PTRACE_CONT executes
 * normally, while the tracer can request the next step explicitly. */
static void debug_exception_handler(struct registers *regs) {
  if ((regs->cs & 0x3) == 0x3) {
    regs->rflags &= ~(1ULL << 8);
    isr_report_user_fault(regs, SIGTRAP, regs->rip);
  } else {
    isr_panic(regs, "Unhandled Debug Exception");
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
  register_interrupt_handler(1, debug_exception_handler);
  register_interrupt_handler(3, breakpoint_handler);
  register_interrupt_handler(4, overflow_handler);
  register_interrupt_handler(6, invalid_opcode_handler);
  register_interrupt_handler(8, double_fault_handler);
  register_interrupt_handler(12, stack_fault_handler);
  register_interrupt_handler(13, gpf_handler);
  register_interrupt_handler(14, page_fault_handler);
  fpu_init(); // Register #NM (vector 7) handler for Lazy FPU switching
}


void isr_handler(struct registers *regs) {
  if (regs->int_no == 0x80) {
    syscall_compat_dispatch(regs);
    return; /* Software syscall: no controller EOI or hardirq accounting. */
  }

  /* Keep per-CPU delivery counts for /proc/interrupts. Count vectors rather
   * than individual handlers: a shared IRQ is one interrupt delivery even
   * when several handlers run for it. */
  if (regs->int_no >= 32 && regs->int_no < 256) {
    struct cpu_info *cpu = cpu_get_current();
    uint32_t cpu_id = cpu ? cpu->cpu_id : 0;
    isr_record_vector_delivery(cpu_id, regs->int_no);
  }

  int is_hw_irq = (regs->int_no >= 32);

  /* Track hardirq context for LinuxKPI's in_interrupt().  CPU exceptions are
   * not interrupt context; only vectors 32+ (IRQs and IPIs) are. */
  extern void linuxkpi_irq_enter(void) __attribute__((weak));
  extern void linuxkpi_irq_exit(void) __attribute__((weak));

  if (is_hw_irq && linuxkpi_irq_enter)
    linuxkpi_irq_enter();

  if (!isr_interrupt_dispatch(regs))
    isr_panic(regs, "Unhandled CPU Exception");

  if (is_hw_irq && linuxkpi_irq_exit)
    linuxkpi_irq_exit();

  /* Signal delivery can stop or terminate this thread and never return to
   * its interrupt frame. Finish EOI and hardirq accounting first: abandoning
   * an in-service device vector blocks lower-priority timer/IPI delivery on
   * this CPU even after the scheduler switches it to idle. */
  if ((regs->cs & 0x3) == 0x3) {
    signal_deliver(regs);
  }

  /* Complete both the controller EOI and LinuxKPI hardirq accounting before
   * a reschedule request can switch away from this interrupt frame. Do not
   * preempt an interrupted IRQ-masked region (including native spinlocks). */
  if (is_hw_irq && (regs->rflags & (1ULL << 9)))
    sched_check_resched((regs->cs & 0x3) == 0x3);

  /* rseq: an IRQ (e.g. the timer) may have preempted a restartable critical
   * section or migrated this thread. Publish the current CPU and abort the
   * section before returning to ring 3. Signal delivery above already ran;
   * if it redirected RIP to a handler, the abort check harmlessly no-ops,
   * and the post-handler resume is covered by the syscall/sigreturn hooks. */
  if ((regs->cs & 0x3) == 0x3) {
    struct thread *rt = sched_get_current();
    if (rt && __builtin_expect(rt->rseq_addr != 0, 0)) {
      rseq_update_cpu_id(rt);
      rseq_maybe_abort(rt, &regs->rip);
    }
  }
}
