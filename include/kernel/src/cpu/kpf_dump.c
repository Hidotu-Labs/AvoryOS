/* Fatal kernel page-fault reporter.
 *
 * When the demand pager refuses a ring-0 fault, all the kernel used to say was
 *
 *     [VMM] KERNEL-mode fault could not be handled by paging engine!
 *
 * and then went quiet: isr_panic() prints to the console and only touches the
 * serial ring buffer in its last statement, which a hung machine never reaches.
 * Two properties of this kernel turn that silence into a debugging dead end:
 *
 *   - klog_* funnels into serial_write(), which takes serial_lock, and the
 *     console path takes the framebuffer lock. Both are ticket spinlocks with
 *     no owner tracking, so if the fault landed while this CPU already held one
 *     of them (quite possible - the fault may have happened *inside* logging),
 *     reporting the panic through those paths re-acquires a lock nobody will
 *     ever release and the box hangs with an empty log.
 *   - an allocation, VMA lookup or file operation in a report can fault again
 *     and turn a #PF into a double fault.
 *
 * So every byte below is pushed straight to the 16550 by serial_write_sync()
 * (no lock, no ring buffer, bounded per-byte wait), every memory read is
 * translated in the faulting address space before it is touched, and nothing is
 * allocated. Output can interleave with other CPUs still draining the serial
 * ring buffer; garbled lines beat no lines.
 */

#include "kpf_dump.h"
#include "../drivers/serial.h"
#include "../mm/pmm.h"
#include "../mm/vmm.h"
#include "../sched/sched.h"
#include "../smp/cpu.h"
#include "../syscalls/syscall.h"
#include "arch/x86_64/extable.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* The image is linked at 0xffffffff80000000 (linker-scripts/x86_64.lds), which
 * exports no _text/_end, so "could be kernel code" is a range test from there. */
#define KERNEL_IMAGE_BASE 0xFFFFFFFF80000000ULL

#define STACK_SCAN_WORDS 512 /* 4 KiB of stack looked over for return addresses */
#define MAX_REPORTED_FRAMES 24

// Output primitives

static void out(const char *s, size_t len) { serial_write_sync(s, len); }

/* For our own literals only: they live in .rodata, so scanning them is safe. */
static void out_lit(const char *s) {
  size_t len = 0;
  while (len < 256 && s[len])
    len++;
  out(s, len);
}

static void out_hexn(uint64_t v, unsigned digits) {
  const char *hex = "0123456789ABCDEF";
  char buf[18];
  for (unsigned i = 0; i < digits; i++)
    buf[i] = hex[(v >> (((digits - 1) - i) * 4)) & 0xF];
  out(buf, digits);
}

static void out_hex(uint64_t v) {
  out_lit("0x");
  out_hexn(v, 16);
}

static void out_dec(uint64_t v) {
  char buf[24];
  int i = (int)sizeof(buf);
  do {
    buf[--i] = (char)('0' + (v % 10));
    v /= 10;
  } while (v != 0 && i > 0);
  out(&buf[i], (size_t)(sizeof(buf) - (size_t)i));
}

static void out_kv(const char *key, uint64_t value) {
  out_lit(" ");
  out_lit(key);
  out_lit("=");
  out_hex(value);
}

// Safe reads
//
// Reading anything the fault may have corrupted faults again, so every access
// is first translated in the *faulting* address space: if the walk does not
// resolve, the byte is unreportable and we say so instead of touching it.

static void *xlate(uint64_t vaddr) {
  /* Raw walk rather than vmm_virt_to_phys(): this runs while reporting a
   * fault that may have corrupted the very page tables the normal walker
   * would chase, and a #GP/#PF inside the reporter turns one dump into a
   * double fault.  The raw walk refuses to follow non-RAM frames. */
  uint64_t phys = vmm_debug_walk((uint64_t)(uintptr_t)vmm_get_active_pml4(),
                                 vaddr, NULL);
  if (phys == 0)
    return NULL;
  return (void *)(uintptr_t)(phys + pmm_get_hhdm_offset());
}

static bool readable(uint64_t vaddr, uint64_t len) {
  if (len == 0 || xlate(vaddr) == NULL)
    return false;
  uint64_t last = vaddr + len - 1;
  if ((last & ~0xFFFULL) != (vaddr & ~0xFFFULL) && xlate(last) == NULL)
    return false;
  return true;
}

static bool read_word(uint64_t vaddr, uint64_t *out_value) {
  if ((vaddr & 7) != 0 || !readable(vaddr, 8))
    return false;
  *out_value = *(volatile uint64_t *)xlate(vaddr);
  return true;
}

/* Strings handed to us by other subsystems (thread names, __FILE__ pointers)
 * can be the corrupted thing themselves: print printable bytes only, from a
 * page we can actually reach. */
static void out_str(const char *s) {
  if (!s) {
    out_lit("-");
    return;
  }
  if (!readable((uint64_t)(uintptr_t)s, 1)) {
    out_lit("<unreadable>");
    return;
  }
  char buf[128];
  size_t n = 0;
  while (n + 1 < sizeof(buf) && s[n] >= 0x20 && s[n] < 0x7F) {
    buf[n] = s[n];
    n++;
  }
  out(buf, n);
  if (n + 1 == sizeof(buf) && s[n] >= 0x20 && s[n] < 0x7F)
    out_lit("...");
}

static const char *region_of(uint64_t addr) {
  if (addr <= USER_SPACE_LIMIT)
    return "user space";
  if (addr < HHDM_BASE)
    return "canonical hole (wild pointer?)";
  if (addr < VMAP_BASE)
    return "HHDM direct map";
  if (addr < KERNEL_HEAP_BASE)
    return "vmmap";
  if (addr < KERNEL_IMAGE_BASE)
    return "kernel heap/slab";
  return "kernel image";
}

/* Only follow a page-table entry whose frame could really be RAM: chasing a
 * corrupted entry is how a diagnostic turns into a double fault. Boot-time
 * tables are not always marked managed, hence the size fallback. */
static bool frame_is_ram(uint64_t phys) {
  if (phys == 0 || (phys & 0xFFF) != 0)
    return false;
  if (pmm_is_managed(phys))
    return true;
  uint64_t total = pmm_get_total_memory();
  return total >= PAGE_SIZE && phys <= total - PAGE_SIZE;
}

static void out_flag(uint64_t entry, unsigned bit, const char *name) {
  out_lit(name);
  out_lit(((entry >> bit) & 1) ? "=1 " : "=0 ");
}

static void out_entry_flags(uint64_t entry, bool leaf) {
  out_flag(entry, 0, "P");
  out_flag(entry, 1, "RW");
  out_flag(entry, 2, "US");
  out_flag(entry, 5, "A");
  out_flag(entry, 6, "D");
  out_flag(entry, 7, leaf ? "PAT" : "PS");
  if (leaf) {
    out_flag(entry, 8, "G");
    out_flag(entry, 9, "COW"); /* this kernel's software copy-on-write marker */
  }
  out_flag(entry, 63, "NX");
}

// The page-table walk for CR2

static void dump_pte_context(uint64_t err_code, uint64_t *table, uint64_t index) {
  out_lit("[KPF]   neighbours ");
  for (uint64_t i = (index > 0 ? index - 1 : 0); i <= index + 1; i++) {
    if (i == index)
      out_lit("<");
    out_hexn(table[i], 16);
    if (i == index)
      out_lit(">");
    out_lit(" ");
  }
  out_lit("\n");

  uint64_t pte = table[index];
  bool said = false;
  if ((err_code & 0x2) && !(pte & PAGE_FLAG_RW)) {
    out_lit("[KPF]   verdict: write fault, but PTE.RW=0\n");
    said = true;
  }
  if ((err_code & 0x4) && !(pte & PAGE_FLAG_USER)) {
    out_lit("[KPF]   verdict: ring-3 access, but PTE.US=0\n");
    said = true;
  }
  if ((err_code & 0x10) && (pte & PAGE_FLAG_NX)) {
    out_lit("[KPF]   verdict: instruction fetch, but PTE.NX=1\n");
    said = true;
  }
  if (pte & PAGE_FLAG_COW) {
    out_lit("[KPF]   verdict: software COW bit is set - the pager declined its "
            "own fixup\n");
    said = true;
  }
  if (!said) {
    out_lit("[KPF]   verdict: PTE is present and permissive - stale TLB, or a "
            "fault the pager declined on purpose\n");
  }
  if (pte & 0x0070000000000000ULL)
    out_lit("[KPF]   note: bits [54:48] are set in the PTE (reserved or "
            "corrupted)\n");
}

static void dump_walk(uint64_t cr2, uint64_t err_code) {
  uint64_t hhdm = pmm_get_hhdm_offset();
  uint64_t cr3 = (uint64_t)(uintptr_t)vmm_get_active_pml4();

  out_lit("[KPF] PAGE WALK of ");
  out_hex(cr2);
  out_lit(" [");
  out_lit(region_of(cr2));
  out_lit("] in CR3=");
  out_hex(cr3);
  out_lit("\n");

  if (!frame_is_ram(cr3)) {
    out_lit("[KPF]   CR3 does not point at RAM - refusing to walk\n");
    return;
  }

  static const char *level_name[4] = {"PML4E", "PDPTE", "PDE", "PTE"};
  uint64_t *table = (uint64_t *)(uintptr_t)(cr3 + hhdm);

  for (unsigned level = 0; level < 4; level++) {
    uint64_t index = (cr2 >> (39 - level * 9)) & 0x1FF;
    uint64_t entry = table[index];

    out_lit("[KPF]   ");
    out_lit(level_name[level]);
    out_lit("[");
    out_dec(index);
    out_lit("] ");
    out_hex(entry);
    out_lit("  ");
    out_entry_flags(entry, level == 3);
    out_lit("\n");

    if (!(entry & PAGE_FLAG_PRESENT)) {
      out_lit("[KPF]   stopped: ");
      out_lit(level_name[level]);
      out_lit(" is not present - nothing is mapped for this address\n");
      if (level == 3)
        dump_pte_context(err_code, table, index);
      return;
    }

    if (level == 3) {
      dump_pte_context(err_code, table, index);
      return;
    }

    if (entry & PAGE_FLAG_PS) {
      if (level == 1)
        out_lit("[KPF]   mapped by a 1GB huge page\n");
      else if (level == 2)
        out_lit("[KPF]   mapped by a 2MB huge page\n");
      else
        out_lit("[KPF]   PS set at this level - page table corruption\n");
      return;
    }

    uint64_t next = entry & PAGE_MASK;
    if (!frame_is_ram(next)) {
      out_lit("[KPF]   entry frame is outside RAM - page table corruption, "
              "refusing to follow it\n");
      return;
    }
    table = (uint64_t *)(uintptr_t)(next + hhdm);
  }
}

static void dump_code_at_rip(uint64_t rip) {
  out_lit("[KPF] CODE AT RIP: ");
  for (int i = -16; i < 16; i++) {
    if (i == 0)
      out_lit("|");
    void *p = xlate(rip + (uint64_t)i);
    if (!p) {
      out_lit("?? ");
      continue;
    }
    out_hexn(*(volatile uint8_t *)p, 2);
    out_lit(" ");
  }
  out_lit("\n");
}

// "How did we get here" - the kernel is built at -O2 without forced frame
// pointers, so a stack scan is the unwind that actually works here.

static void dump_stack_scan(uint64_t sp, const struct thread *t) {
  uint64_t lo = sp & ~7ULL;
  uint64_t hi = 0;

  /* Stop at the end of the stack we faulted on rather than wandering into
   * whatever happens to live above it. */
  if (t && t->stack_size != 0 && lo >= t->stack_base &&
      lo < t->stack_base + t->stack_size) {
    hi = t->stack_base + t->stack_size;
  } else {
    struct cpu_info *c = cpu_get_current();
    if (readable((uint64_t)(uintptr_t)c, sizeof(void *)) && c->self == c &&
        lo < c->stack_top && c->stack_top <= lo + CPU_STACK_SIZE)
      hi = c->stack_top;
  }
  if (hi < lo || hi > lo + STACK_SCAN_WORDS * 8)
    hi = lo + STACK_SCAN_WORDS * 8;

  out_lit("[KPF] STACK SCAN from sp=");
  out_hex(lo);
  out_lit(" for kernel-text values (candidate return addresses):\n");

  unsigned found = 0;
  for (uint64_t addr = lo; addr < hi; addr += 8) {
    uint64_t value;
    if (!read_word(addr, &value))
      continue;
    if (value < KERNEL_IMAGE_BASE)
      continue;

    out_lit("[KPF]   sp+0x");
    out_hexn(addr - lo, 4);
    out_lit(" -> ");
    out_hex(value);
    out_lit(" [");
    out_lit(region_of(value));
    out_lit("]");
    if (!readable(value, 1))
      out_lit(" NOT MAPPED");
    out_lit("\n");

    if (++found >= MAX_REPORTED_FRAMES) {
      out_lit("[KPF]   ... truncated\n");
      break;
    }
  }
  if (found == 0)
    out_lit("[KPF]   nothing that looks like a kernel return address\n");
}

static void dump_rbp_chain(uint64_t rbp) {
  out_lit("[KPF] RBP CHAIN (best effort):\n");
  for (int frame = 0; frame < 16; frame++) {
    uint64_t saved_rbp = 0, ret = 0;
    if (!read_word(rbp, &saved_rbp) || !read_word(rbp + 8, &ret)) {
      out_lit("[KPF]   #");
      out_dec((uint64_t)frame);
      out_lit(": unreadable frame at rbp=");
      out_hex(rbp);
      out_lit("\n");
      return;
    }
    out_lit("[KPF]   #");
    out_dec((uint64_t)frame);
    out_lit(": ret=");
    out_hex(ret);
    out_lit(" [");
    out_lit(region_of(ret));
    out_lit("]\n");
    if (saved_rbp <= rbp || ret < KERNEL_IMAGE_BASE)
      return;
    rbp = saved_rbp;
  }
}

// Machine and software context

static void dump_error_code(uint64_t err_code) {
  out_lit("[KPF] ERROR CODE ");
  out_hex(err_code);
  out_lit(" ");
  out_flag(err_code, 0, "P");
  out_flag(err_code, 1, "W");
  out_flag(err_code, 2, "U");
  out_flag(err_code, 3, "RSVD");
  out_flag(err_code, 4, "I");
  out_flag(err_code, 5, "PK");
  out_flag(err_code, 6, "SS");
  out_lit("\n[KPF] CAUSE: ");
  out_lit((err_code & 0x10) ? "instruction fetch" : (err_code & 0x2) ? "write"
                                                                     : "read");
  out_lit(" of ");
  out_lit((err_code & 0x1) ? "a present page (protection violation)"
                           : "a non-present page");
  out_lit(" from ring ");
  out_dec((err_code & 0x4) ? 3 : 0);
  out_lit("\n");
  if (err_code & 0x8)
    out_lit("[KPF] NOTE: RSVD set - a page-table entry has reserved bits "
            "populated (corruption, or a software bit the CPU rejects)\n");
}

static void dump_machine_state(struct registers *regs, uint64_t cr2,
                               uint64_t live_rsp) {
  uint64_t cr0 = 0, cr3 = 0, cr4 = 0;
  __asm__ volatile("mov %%cr0, %0" : "=r"(cr0));
  __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
  __asm__ volatile("mov %%cr4, %0" : "=r"(cr4));

  out_lit("[KPF] RIP=");
  out_hex(regs->rip);
  out_lit(" [");
  out_lit(region_of(regs->rip));
  out_lit("] CS=");
  out_hexn(regs->cs, 4);
  out_lit(" RFLAGS=");
  out_hex(regs->rflags);
  out_lit(" IF=");
  out_lit(((regs->rflags >> 9) & 1) ? "1" : "0");
  /* Worth spelling out: with DF=1 every memset/memcpy in this kernel is a REP
   * string counting downwards, so a string operation writes below its
   * destination instead of above it. */
  out_lit(" DF=");
  out_lit(((regs->rflags >> 10) & 1) ? "1 (REP strings run backwards!)" : "0");
  out_lit("\n[KPF] RSP=");
  out_hex(live_rsp);
  out_lit(" (live snapshot: the CPU does not save RSP for a ring-0 fault)\n");

  out_lit("[KPF] CR0=");
  out_hex(cr0);
  out_lit(" CR3=");
  out_hex(cr3);
  out_lit(" CR4=");
  out_hex(cr4);
  out_lit("\n[KPF]      ");
  out_flag(cr0, 0, "PE");
  out_flag(cr0, 16, "WP");
  out_flag(cr0, 31, "PG");
  out_flag(cr4, 20, "SMEP");
  out_flag(cr4, 21, "SMAP");
  out_lit("\n[KPF] CR2=");
  out_hex(cr2);
  out_lit(" [");
  out_lit(region_of(cr2));
  out_lit("]\n");

  if (!(regs->cs & 0x3) && cr2 <= USER_SPACE_LIMIT)
    out_lit("[KPF] NOTE: kernel code touched a user address - SMAP violation or "
            "a missing copy_to/from_user guard\n");

  out_lit("[KPF] REGISTERS:\n[KPF]   RAX=");
  out_hex(regs->rax);
  out_kv("RBX", regs->rbx);
  out_kv("RCX", regs->rcx);
  out_kv("RDX", regs->rdx);
  out_lit("\n[KPF]   RSI=");
  out_hex(regs->rsi);
  out_kv("RDI", regs->rdi);
  out_kv("RBP", regs->rbp);
  out_lit("\n[KPF]   R8 =");
  out_hex(regs->r8);
  out_kv("R9", regs->r9);
  out_kv("R10", regs->r10);
  out_kv("R11", regs->r11);
  out_lit("\n[KPF]   R12=");
  out_hex(regs->r12);
  out_kv("R13", regs->r13);
  out_kv("R14", regs->r14);
  out_kv("R15", regs->r15);
  out_lit("\n");
}

static void dump_thread_context(void) {
  struct cpu_info *c = cpu_get_current();
  struct thread *t = sched_get_current();

  out_lit("[KPF] CPU=");
  if (readable((uint64_t)(uintptr_t)c, sizeof(void *)) && c->self == c) {
    out_dec(c->cpu_id);
    out_lit(" (lapic ");
    out_dec(c->apic_id);
    out_lit(")");
  } else {
    out_lit("unknown");
  }
  if (!t) {
    out_lit(" THREAD: none (fault outside any thread context)\n");
    return;
  }

  out_lit(" THREAD: tid=");
  out_dec(t->tid);
  out_lit(" tgid=");
  out_dec(t->tgid);
  out_lit(" comm='");
  out_str(t->comm);
  out_lit("' state=");
  out_dec((uint64_t)t->state);
  out_lit("\n[KPF] KERNEL SITE: ");
  out_str(t->last_kernel_file);
  out_lit(":");
  out_dec(t->last_kernel_line);
  out_lit(" ");
  out_str(t->last_kernel_func);
  out_lit(" subsys=");
  out_str(t->last_subsystem);
  out_kv("last_err", (uint64_t)t->last_error_code);
  out_lit("\n[KPF] LAST SYSCALL: ");
  out_str(syscall_get_name(t->last_syscall_num));
  out_lit(" (");
  out_dec(t->last_syscall_num);
  out_lit(") -> ");
  out_hex((uint64_t)t->last_syscall_ret);
  out_lit(" args=");
  out_hex(t->last_syscall_args[0]);
  out_lit(" ");
  out_hex(t->last_syscall_args[1]);
  out_lit(" ");
  out_hex(t->last_syscall_args[2]);
  out_lit("\n[KPF] CR3(thread)=");
  out_hex(t->cr3);
  out_lit(" mm=");
  out_hex((uint64_t)(uintptr_t)t->mm);
  out_lit(" kstack=[");
  out_hex(t->stack_base);
  out_lit("..");
  out_hex(t->stack_base + t->stack_size);
  out_lit("]\n");
}

static void dump_pager_verdict(uint64_t cr2) {
  struct vmm_fault_reject r;

  out_lit("[KPF] PAGER VERDICT: ");
  if (!vmm_get_last_fault_reject(&r)) {
    out_lit("the paging engine never reported a reason (it bailed out before "
            "recording one)\n");
    return;
  }
  if (r.cr2 != cr2) {
    out_lit("no rejection matches this fault; the newest one was for CR2=");
    out_hex(r.cr2);
    out_lit(" (seq ");
    out_dec(r.seq);
    out_lit(", tid ");
    out_dec(r.tid);
    out_lit(") from another CPU\n");
    return;
  }
  out_lit(r.reason);
  out_lit("\n[KPF]   refused at ");
  out_str(r.file);
  out_lit(":");
  out_dec(r.line);
  out_lit(" seq=");
  out_dec(r.seq);
  out_lit(" tid=");
  out_dec(r.tid);
  out_lit("\n[KPF]   context: cr2=");
  out_hex(r.cr2);
  out_lit(" err=");
  out_hexn(r.err_code, 4);
  out_lit(" rip=");
  out_hex(r.rip);
  out_lit(" detail=");
  out_hex(r.detail);
  out_lit(" detail2=");
  out_hex(r.detail2);
  out_lit("\n");
}

// Entry points

void kpf_dump_page_fault(struct registers *regs, uint64_t cr2) {
  if (!regs)
    return;

  /* Report once and stick to it. A refused kernel fault usually cascades - the
   * console dump faults too - and reprinting the whole thing each time buries
   * the single dump that explains the crash. */
  static volatile unsigned reported;
  if (__atomic_exchange_n(&reported, 1, __ATOMIC_ACQ_REL)) {
    out_lit("\n[KPF] faulted again while reporting - the dump above is the one "
            "to read\n");
    out_lit("[KPF] second fault: cr2=");
    out_hex(cr2);
    out_lit(" err=");
    out_hexn(regs->err_code, 4);
    out_lit(" rip=");
    out_hex(regs->rip);
    out_lit(" [");
    out_lit(region_of(regs->rip));
    out_lit("]\n");
    return;
  }

  uint64_t live_rsp;
  __asm__ volatile("mov %%rsp, %0" : "=r"(live_rsp));

  out_lit("\n[KPF] +----------------------------------------------------------"
          "------------------+\n");
  out_lit("[KPF] | KERNEL PAGE FAULT the paging engine refused - panicking, "
          "this is fatal    |\n");
  out_lit("[KPF] +----------------------------------------------------------"
          "------------------+\n");

  dump_pager_verdict(cr2);
  dump_error_code(regs->err_code);
  dump_machine_state(regs, cr2, live_rsp);
  dump_thread_context();
  out_lit("[KPF] FIXUP TABLE: RIP ");
  out_lit(extable_has_entry(regs->rip) ? "has an extable entry that did not "
                                         "apply to this fault\n"
                                       : "has no extable fixup entry\n");
  dump_walk(cr2, regs->err_code);
  dump_code_at_rip(regs->rip);
  dump_stack_scan(live_rsp, sched_get_current());
  dump_rbp_chain(regs->rbp);

  out_lit("[KPF] report complete - console panic banner follows (screen "
          "only)\n");
}

void kpf_dump_panic_entry(const char *reason, struct registers *regs) {
  out_lit("\n[PANIC] ");
  out_lit(reason ? reason : "<no reason given>");
  if (regs) {
    out_lit(" int=");
    out_dec(regs->int_no);
    out_lit(" err=");
    out_hexn(regs->err_code, 4);
    out_lit(" rip=");
    out_hex(regs->rip);
    out_lit(" [");
    out_lit(region_of(regs->rip));
    out_lit("]");
    struct thread *t = sched_get_current();
    if (t) {
      out_lit(" tid=");
      out_dec(t->tid);
      out_lit(" comm='");
      out_str(t->comm);
      out_lit("'");
    }
  }
  out_lit("\n");
}

void kpf_dump_panic_recursion(const char *reason, struct registers *regs) {
  out_lit("\n[PANIC] recursed while panicking");
  out_lit(reason ? ": " : "\n");
  if (reason)
    out_lit(reason);
  if (regs) {
    out_lit(" int=");
    out_dec(regs->int_no);
    out_lit(" rip=");
    out_hex(regs->rip);
    out_lit(" [");
    out_lit(region_of(regs->rip));
    out_lit("]");
  }
  out_lit("\n[PANIC] the console path is unusable, halting here - the [KPF] "
          "report above is the useful one\n");
}
