#include "svm.h"

#include "avm_cpuid.h"
#include "avm_ioeventfd.h"
#include "avm_irq.h"
#include "avm_lapic.h"
#include "avm_mmio.h"
#include "avm_msr.h"
#include "avm_smp.h"

#include "../apic/lapic_timer.h"
#include "../console/klog.h"
#include "../sched/sched.h"
#include "../cpu/features.h"
#include "../cpu/msr.h"
#include "../hal/hal.h"
#include "../include/linux/errno.h"
#include "../lib/string.h"
#include "../mm/pmm.h"
#include "../mm/vmm.h"
#include "../smp/cpu.h"

#define CR4_OSXSAVE (1ull << 18)

/* Permission maps are physically contiguous.  The buddy allocator takes an
 * exact page count here, so these are the sizes the hardware needs. */
#define IOPM_PAGES 3   /* 12 KB */
#define MSRPM_PAGES 2  /* 8 KB */

/* Where IN/OUT data sits inside the kvm_run page: past the fixed header and
 * the exit union. */
#define AVM_IO_DATA_OFFSET 2048

#define VMCB_SAVE_CR2 0x540

#define RFLAGS_IF (1ull << 9)
#define RFLAGS_TF (1ull << 8)
#define RFLAGS_DF (1ull << 10)

/* IOIO exit information. */
#define IOIO_IN (1ull << 0)
#define IOIO_STRING (1ull << 2)
#define IOIO_REP (1ull << 3)
#define IOIO_SIZE8 (1ull << 4)
#define IOIO_SIZE16 (1ull << 5)
#define IOIO_ADDR16 (1ull << 7)
#define IOIO_ADDR32 (1ull << 8)
#define IOIO_SEG_SHIFT 10

#define VQ(v, off) (*(volatile uint64_t *)((uint8_t *)(v) + (off)))
#define VD(v, off) (*(volatile uint32_t *)((uint8_t *)(v) + (off)))
#define VW(v, off) (*(volatile uint16_t *)((uint8_t *)(v) + (off)))
#define VB(v, off) (*(volatile uint8_t *)((uint8_t *)(v) + (off)))

/* Implemented in svm_run.asm.  Interrupts must be off on entry. */
void svm_run_vcpu(uint64_t guest_vmcb_pa, uint64_t host_vmcb_pa,
                  struct kvm_regs *gprs);

static uint64_t host_save_pa[MAX_CPUS];
static uint64_t host_vmcb_pa[MAX_CPUS];
/* Each CPU's host FPU/SIMD save area, used around every VMRUN.  Static rather
 * than from the page allocator, to tell an allocator fault from a stray write:
 * the page was seen holding kernel pointers. */
static uint8_t host_xsave_area[MAX_CPUS][4096] __attribute__((aligned(4096)));
static bool cpu_ready[MAX_CPUS];

static uint32_t xsave_pages_needed(void);

static uint8_t *hhdm(uint64_t pa) {
  return (uint8_t *)(pa + pmm_get_hhdm_offset());
}

bool svm_supported(void) {
  uint32_t a, b, c, d;

  svm_cpuid(0x80000000u, 0, &a, &b, &c, &d);
  if (a < 0x8000000Au) {
    klog_puts("[AVM] CPUID lacks SVM leaves; SVM unavailable\n");
    return false;
  }

  svm_cpuid(0x80000001u, 0, &a, &b, &c, &d);
  if (!(c & CPUID_80000001_ECX_SVM)) {
    klog_puts("[AVM] CPU does not report SVM\n");
    return false;
  }

  svm_cpuid(0x8000000Au, 0, &a, &b, &c, &d);
  if (!(d & CPUID_8000000A_EDX_NRIPS)) {
    klog_puts("[AVM] CPU lacks next-RIP save; SVM unavailable\n");
    return false;
  }
  avm_smp_set_asid_count(b);  /* EBX: number of ASIDs */

  uint64_t vm_cr = rdmsr(MSR_VM_CR);
  if (vm_cr & VM_CR_SVMDIS) {
    klog_puts((vm_cr & VM_CR_LOCK)
                  ? "[AVM] SVM disabled and locked by firmware (VM_CR)\n"
                  : "[AVM] SVM disabled by VM_CR.SVMDIS\n");
    return false;
  }

  return true;
}

int svm_cpu_enable(void) {
  uint32_t id = cpu_get_current()->cpu_id;
  if (id >= MAX_CPUS)
    return -EINVAL;

  /* Each CPU owns its host save page and its VMSAVE/VMLOAD page. */
  if (!host_save_pa[id]) {
    uint64_t pa = (uint64_t)pmm_alloc();
    if (!pa)
      return -ENOMEM;
    memset(hhdm(pa), 0, PAGE_SIZE);
    host_save_pa[id] = pa;
  }
  if (!host_vmcb_pa[id]) {
    uint64_t pa = (uint64_t)pmm_alloc();
    if (!pa)
      return -ENOMEM;
    memset(hhdm(pa), 0, PAGE_SIZE);
    host_vmcb_pa[id] = pa;
  }
  /* The static area holds one page, which is enough for the XSAVE state. */
  if (xsave_pages_needed() > 1)
    return -EINVAL;

  wrmsr(MSR_VM_HSAVE_PA, host_save_pa[id]);
  wrmsr(MSR_EFER, rdmsr(MSR_EFER) | EFER_SVME);
  cpu_ready[id] = true;
  return 0;
}

int svm_vcpu_init(struct avm_vcpu *vcpu) {
  uint64_t vmcb = (uint64_t)pmm_alloc();
  uint64_t iopm = (uint64_t)pmm_alloc_pages(IOPM_PAGES);
  uint64_t msrpm = (uint64_t)pmm_alloc_pages(MSRPM_PAGES);
  if (!vmcb || !iopm || !msrpm) {
    if (vmcb)
      pmm_free((void *)vmcb);
    if (iopm)
      pmm_free_pages((void *)iopm, IOPM_PAGES);
    if (msrpm)
      pmm_free_pages((void *)msrpm, MSRPM_PAGES);
    return -ENOMEM;
  }

  memset(hhdm(vmcb), 0, PAGE_SIZE);
  /* All bits set: every port and MSR exits to the hypervisor. */
  memset(hhdm(iopm), 0xFF, IOPM_PAGES * PAGE_SIZE);
  memset(hhdm(msrpm), 0xFF, MSRPM_PAGES * PAGE_SIZE);

  uint8_t *v = hhdm(vmcb);
  VD(v, VMCB_INTERCEPT_MISC1) = INTERCEPT_INTR | INTERCEPT_NMI | INTERCEPT_CPUID |
                                INTERCEPT_HLT | INTERCEPT_IOIO |
                                INTERCEPT_MSR | INTERCEPT_SHUTDOWN;
  VD(v, VMCB_INTERCEPT_MISC2) = INTERCEPT_VMRUN | INTERCEPT_XSETBV;
  VQ(v, VMCB_IOPM_BASE_PA) = iopm;
  VQ(v, VMCB_MSRPM_BASE_PA) = msrpm;
  VD(v, VMCB_GUEST_ASID) = vcpu->vm->asid;
  VQ(v, VMCB_NP_ENABLE) = 1;
  VQ(v, VMCB_N_CR3) = vcpu->vm->npt_root;
  VD(v, VMCB_CLEAN_BITS) = 0;
  /* The guest's PAT starts at the reset value. */
  VQ(v, VMCB_SAVE_G_PAT) = 0x0007040600070406ull;

  uint32_t xpages = xsave_pages_needed();
  uint64_t xsave = 0;
  if (xpages) {
    xsave = (uint64_t)pmm_alloc_pages(xpages);
    if (!xsave) {
      pmm_free((void *)vmcb);
      pmm_free_pages((void *)iopm, IOPM_PAGES);
      pmm_free_pages((void *)msrpm, MSRPM_PAGES);
      return -ENOMEM;
    }
    memset(hhdm(xsave), 0, xpages * PAGE_SIZE);
  }

  vcpu->vmcb_pa = vmcb;
  vcpu->iopm_pa = iopm;
  vcpu->msrpm_pa = msrpm;
  vcpu->xsave_pa = xsave;
  vcpu->xcr0 = 1;  /* x87 only, as after reset */
  vcpu->last_apic_id = 0xFFFFFFFFu;
  vcpu->state_dirty = true;
  return 0;
}

void svm_vcpu_destroy(struct avm_vcpu *vcpu) {
  if (vcpu->vmcb_pa)
    pmm_free((void *)vcpu->vmcb_pa);
  if (vcpu->iopm_pa)
    pmm_free_pages((void *)vcpu->iopm_pa, IOPM_PAGES);
  if (vcpu->msrpm_pa)
    pmm_free_pages((void *)vcpu->msrpm_pa, MSRPM_PAGES);
  if (vcpu->xsave_pa)
    pmm_free_pages((void *)vcpu->xsave_pa, xsave_pages_needed());
  vcpu->vmcb_pa = vcpu->iopm_pa = vcpu->msrpm_pa = vcpu->xsave_pa = 0;
}

/* Writes one segment in VMCB format: selector, 12-bit attributes (type,
 * S, DPL, P, AVL, L, DB, G), limit, base. */
static void seg_in(uint8_t *v, uint32_t off, const struct kvm_segment *s) {
  uint16_t attrib = 0;
  if (!s->unusable)
    attrib = (uint16_t)((s->type & 0xF) | (s->s << 4) | ((s->dpl & 3) << 5) |
                        (s->present << 7) | (s->avl << 8) | (s->l << 9) |
                        (s->db << 10) | (s->g << 11));
  VW(v, off) = s->selector;
  VW(v, off + 2) = attrib;
  VD(v, off + 4) = s->limit;
  VQ(v, off + 8) = s->base;
}

static void dt_in(uint8_t *v, uint32_t off, const struct kvm_dtable *d) {
  VD(v, off + 4) = d->limit;
  VQ(v, off + 8) = d->base;
}

/* Writes the full guest state from the vCPU's saved registers and special
 * registers.  Runs only after KVM_SET_REGS or KVM_SET_SREGS. */
static void state_in(struct avm_vcpu *vcpu) {
  uint8_t *v = hhdm(vcpu->vmcb_pa);
  const struct kvm_sregs *s = &vcpu->sregs;

  seg_in(v, VMCB_SAVE_ES, &s->es);
  seg_in(v, VMCB_SAVE_CS, &s->cs);
  seg_in(v, VMCB_SAVE_SS, &s->ss);
  seg_in(v, VMCB_SAVE_DS, &s->ds);
  seg_in(v, VMCB_SAVE_FS, &s->fs);
  seg_in(v, VMCB_SAVE_GS, &s->gs);
  seg_in(v, VMCB_SAVE_TR, &s->tr);
  seg_in(v, VMCB_SAVE_LDTR, &s->ldt);
  dt_in(v, VMCB_SAVE_GDTR, &s->gdt);
  dt_in(v, VMCB_SAVE_IDTR, &s->idt);

  VB(v, VMCB_SAVE_CPL) = (uint8_t)(s->cs.dpl & 3);
  /* VMRUN rejects a guest without SVME in its EFER. */
  VQ(v, VMCB_SAVE_EFER) = s->efer | EFER_SVME;
  VQ(v, VMCB_SAVE_CR0) = s->cr0;
  VQ(v, VMCB_SAVE_CR2) = s->cr2;
  VQ(v, VMCB_SAVE_CR3) = s->cr3;
  VQ(v, VMCB_SAVE_CR4) = s->cr4;
  VQ(v, VMCB_SAVE_DR6) = 0xFFFF0FF0ull;
  VQ(v, VMCB_SAVE_DR7) = 0x400;
}

static void seg_out(uint8_t *v, uint32_t off, struct kvm_segment *s) {
  uint16_t attrib = VW(v, off + 2);
  s->selector = VW(v, off);
  s->type = attrib & 0xF;
  s->s = (attrib >> 4) & 1;
  s->dpl = (attrib >> 5) & 3;
  s->present = (attrib >> 7) & 1;
  s->avl = (attrib >> 8) & 1;
  s->l = (attrib >> 9) & 1;
  s->db = (attrib >> 10) & 1;
  s->g = (attrib >> 11) & 1;
  s->unusable = attrib == 0;
  s->padding = 0;
  s->limit = VD(v, off + 4);
  s->base = VQ(v, off + 8);
}

static void dt_out(uint8_t *v, uint32_t off, struct kvm_dtable *d) {
  d->limit = (uint16_t)VD(v, off + 4);
  d->base = VQ(v, off + 8);
  d->padding[0] = 0;
  d->padding[1] = 0;
  d->padding[2] = 0;
}

/* The special registers as the guest left them: the VMCB holds them after
 * each exit.  Only valid once the vCPU has entered and has no state pending. */
void svm_sregs_out(struct avm_vcpu *vcpu, struct kvm_sregs *s) {
  uint8_t *v = hhdm(vcpu->vmcb_pa);

  seg_out(v, VMCB_SAVE_ES, &s->es);
  seg_out(v, VMCB_SAVE_CS, &s->cs);
  seg_out(v, VMCB_SAVE_SS, &s->ss);
  seg_out(v, VMCB_SAVE_DS, &s->ds);
  seg_out(v, VMCB_SAVE_FS, &s->fs);
  seg_out(v, VMCB_SAVE_GS, &s->gs);
  seg_out(v, VMCB_SAVE_TR, &s->tr);
  seg_out(v, VMCB_SAVE_LDTR, &s->ldt);
  dt_out(v, VMCB_SAVE_GDTR, &s->gdt);
  dt_out(v, VMCB_SAVE_IDTR, &s->idt);

  s->cr0 = VQ(v, VMCB_SAVE_CR0);
  s->cr2 = VQ(v, VMCB_SAVE_CR2);
  s->cr3 = VQ(v, VMCB_SAVE_CR3);
  s->cr4 = VQ(v, VMCB_SAVE_CR4);
  s->cr8 = 0;
  s->efer = VQ(v, VMCB_SAVE_EFER) & ~EFER_SVME;
  memset(s->interrupt_bitmap, 0, sizeof(s->interrupt_bitmap));

  avm_irq_lock(vcpu->vm);
  uint64_t base = 0;
  if (avm_lapic_rdmsr(vcpu, MSR_APIC_BASE, &base) == 0)
    s->apic_base = base;
  avm_irq_unlock(vcpu->vm);
}

/* The general registers VMRUN reads from the VMCB.  RAX, RSP, RIP and RFLAGS
 * are copied here; the other GPRs go through svm_run_vcpu. */
static void gprs_in(struct avm_vcpu *vcpu) {
  uint8_t *v = hhdm(vcpu->vmcb_pa);
  VQ(v, VMCB_SAVE_RAX) = vcpu->regs.rax;
  VQ(v, VMCB_SAVE_RSP) = vcpu->regs.rsp;
  VQ(v, VMCB_SAVE_RIP) = vcpu->regs.rip;
  /* Single-step sets TF and traps each instruction (#DB, intercepted).  The
   * VMM never sees TF in the registers it reads back. */
  VQ(v, VMCB_SAVE_RFLAGS) = vcpu->regs.rflags | 2 | (vcpu->singlestep ? RFLAGS_TF : 0);
  VD(v, VMCB_INTERCEPT_EXC) = vcpu->singlestep ? EXCP_DB_INTERCEPT : 0;
}

static void gprs_out(struct avm_vcpu *vcpu) {
  uint8_t *v = hhdm(vcpu->vmcb_pa);
  vcpu->regs.rax = VQ(v, VMCB_SAVE_RAX);
  vcpu->regs.rsp = VQ(v, VMCB_SAVE_RSP);
  vcpu->regs.rip = VQ(v, VMCB_SAVE_RIP);
  vcpu->regs.rflags = VQ(v, VMCB_SAVE_RFLAGS) & ~(vcpu->singlestep ? RFLAGS_TF : 0);
}

/* Puts a value from the VMM's IN reply into the low bytes of RAX.  A 32-bit
 * IN zero-extends, as on hardware. */
static void apply_io_in(struct avm_vcpu *vcpu) {
  uint64_t value = 0;
  memcpy(&value, (uint8_t *)vcpu->run + AVM_IO_DATA_OFFSET, vcpu->io_in_size);
  switch (vcpu->io_in_size) {
  case 1:
    vcpu->regs.rax = (vcpu->regs.rax & ~0xFFull) | (value & 0xFF);
    break;
  case 2:
    vcpu->regs.rax = (vcpu->regs.rax & ~0xFFFFull) | (value & 0xFFFF);
    break;
  default:
    vcpu->regs.rax = value & 0xFFFFFFFFull;
    break;
  }
  vcpu->io_in_pending = false;
}

/* A PIO write to a registered ioeventfd signals it, and the guest resumes. */
static bool try_ioeventfd_pio(struct avm_vcpu *vcpu, uint8_t *v) {
  uint64_t info = VQ(v, VMCB_EXITINFO1);
  if (info & (IOIO_STRING | IOIO_REP | IOIO_IN))
    return false;
  uint32_t size = (info & (1u << 4)) ? 1 : (info & (1u << 5)) ? 2 : 4;
  uint64_t value = VQ(v, VMCB_SAVE_RAX) & ((1ull << (size * 8)) - 1);
  if (!avm_ioeventfd_write(vcpu->vm, true, (uint16_t)(info >> 16), size, value))
    return false;
  vcpu->regs.rip = VQ(v, VMCB_NRIP);
  avm_step_done(vcpu);
  return true;
}

/* PIC and PIT ports are handled here, so the guest never exits for them.
 * Returns false for anything else, including string and REP forms. */
static bool try_inkernel_io(struct avm_vcpu *vcpu, uint8_t *v) {
  uint64_t info = VQ(v, VMCB_EXITINFO1);
  if (info & (IOIO_STRING | IOIO_REP))
    return false;
  if (!(info & IOIO_SIZE8))
    return false;

  bool in = info & IOIO_IN;
  uint16_t port = (uint16_t)(info >> 16);
  uint8_t value = in ? 0 : (uint8_t)vcpu->regs.rax;
  if (!avm_irq_io(vcpu->vm, port, in, &value))
    return false;

  if (in)
    vcpu->regs.rax = (vcpu->regs.rax & ~0xFFull) | value;
  vcpu->regs.rip = VQ(v, VMCB_NRIP);
  avm_step_done(vcpu);
  return true;
}

/* Linear base of a segment register, from the VMCB save area. */
static uint64_t seg_base(uint8_t *v, uint32_t seg) {
  static const uint64_t save[] = {VMCB_SAVE_ES, VMCB_SAVE_CS, VMCB_SAVE_SS,
                                  VMCB_SAVE_DS, VMCB_SAVE_FS, VMCB_SAVE_GS};
  return VQ(v, save[seg] + 8);
}

#define AVM_STR_ERROR (-1)
#define AVM_STR_TO_VMM 0
#define AVM_STR_IN_KERNEL 1

/* String I/O (INS and OUTS, with or without REP).  One exit moves as many
 * elements as fit in the run page.  RCX (under REP), RSI or RDI advance now,
 * and RIP stays on the instruction while REP has elements left.  INS data is
 * written to guest memory on re-entry.  The kernel reads and writes guest
 * memory directly, so guest paging must be off, as it is in real mode. */
static int fill_string_io(struct avm_vcpu *vcpu, uint8_t *v, uint64_t info) {
  struct kvm_run *run = vcpu->run;
  bool in = info & IOIO_IN;
  bool rep = info & IOIO_REP;
  uint64_t size = (info & IOIO_SIZE8) ? 1 : (info & IOIO_SIZE16) ? 2 : 4;
  uint64_t mask = (info & IOIO_ADDR16) ? 0xFFFFull
                  : (info & IOIO_ADDR32) ? 0xFFFFFFFFull
                                         : ~0ull;
  /* INS always writes ES:DI.  OUTS reads from the segment the SEG field names. */
  uint32_t seg = in ? 0 : (info >> IOIO_SEG_SHIFT) & 7;
  if (seg > 5)
    return AVM_STR_ERROR;
  if (VQ(v, VMCB_SAVE_CR0) & CR0_PG) {
    klog_puts("[AVM] string I/O with paging on is not emulated\n");
    return AVM_STR_ERROR;
  }

  uint64_t *idx_reg = in ? &vcpu->regs.rdi : &vcpu->regs.rsi;
  uint64_t start = *idx_reg & mask;
  uint64_t count = rep ? (vcpu->regs.rcx & mask) : 1;
  uint64_t cap = (AVM_PAGE_SIZE - AVM_IO_DATA_OFFSET) / size;
  if (count > cap)
    count = cap;
  uint64_t rip = VQ(v, VMCB_SAVE_RIP);
  uint64_t next_rip = VQ(v, VMCB_NRIP);
  if (count == 0) {
    vcpu->regs.rip = next_rip;
    avm_step_done(vcpu);
    return AVM_STR_IN_KERNEL;
  }

  uint64_t base = seg_base(v, seg);
  uint64_t step = (VQ(v, VMCB_SAVE_RFLAGS) & RFLAGS_DF) ? 0 - size : size;
  if (!in) {
    uint8_t *data = (uint8_t *)run + AVM_IO_DATA_OFFSET;
    for (uint64_t i = 0; i < count; i++) {
      uint64_t off = (start + i * step) & mask;
      if (avm_mmio_guest_read(vcpu->vm, base + off, data + i * size, size)) {
        klog_puts("[AVM] OUTS source is not in a memslot\n");
        return AVM_STR_ERROR;
      }
    }
  }

  avm_set_addr_reg(idx_reg, mask, start + count * step);
  if (rep)
    avm_set_addr_reg(&vcpu->regs.rcx, mask, (vcpu->regs.rcx & mask) - count);
  bool more = rep && (vcpu->regs.rcx & mask) != 0;
  vcpu->regs.rip = more ? rip : next_rip;

  run->exit_reason = KVM_EXIT_IO;
  run->io.direction = in ? KVM_EXIT_IO_IN : KVM_EXIT_IO_OUT;
  run->io.size = (uint32_t)size;
  run->io.port = (uint16_t)(info >> 16);
  run->io.count = (uint32_t)count;
  run->io.data_offset = AVM_IO_DATA_OFFSET;
  if (in) {
    vcpu->io_str_pending = true;
    vcpu->io_str_size = (uint8_t)size;
    vcpu->io_str_count = count;
    vcpu->io_str_base = base;
    vcpu->io_str_start = start;
    vcpu->io_str_step = step;
    vcpu->io_str_mask = mask;
  }
  avm_step_done(vcpu);
  return AVM_STR_TO_VMM;
}

/* Writes the VMM's INS data into guest memory, on re-entry after the exit. */
static bool apply_string_in(struct avm_vcpu *vcpu) {
  const uint8_t *data = (const uint8_t *)vcpu->run + AVM_IO_DATA_OFFSET;
  vcpu->io_str_pending = false;
  for (uint64_t i = 0; i < vcpu->io_str_count; i++) {
    uint64_t off = (vcpu->io_str_start + i * vcpu->io_str_step) & vcpu->io_str_mask;
    if (avm_mmio_guest_write(vcpu->vm, vcpu->io_str_base + off,
                             data + i * vcpu->io_str_size, vcpu->io_str_size))
      return false;
  }
  return true;
}

static void fill_io_exit(struct avm_vcpu *vcpu, uint8_t *v) {
  struct kvm_run *run = vcpu->run;
  uint64_t info = VQ(v, VMCB_EXITINFO1);
  bool in = info & 1;
  uint8_t size = (info & (1u << 4)) ? 1 : (info & (1u << 5)) ? 2 : 4;

  run->exit_reason = KVM_EXIT_IO;
  run->io.direction = in ? KVM_EXIT_IO_IN : KVM_EXIT_IO_OUT;
  run->io.size = size;
  run->io.port = (uint16_t)(info >> 16);
  run->io.count = 1;
  run->io.data_offset = AVM_IO_DATA_OFFSET;

  if (in) {
    vcpu->io_in_pending = true;
    vcpu->io_in_size = size;
  } else {
    uint64_t value = VQ(v, VMCB_SAVE_RAX);
    memcpy((uint8_t *)run + AVM_IO_DATA_OFFSET, &value, size);
  }
  vcpu->regs.rip = VQ(v, VMCB_NRIP);
  avm_step_done(vcpu);
}

/* CPUID from the vCPU's table.  Leaf 1 ECX.OSXSAVE follows the guest's CR4,
 * as it does on hardware, because the guest only sees XSAVE once it has
 * enabled it. */
static void handle_cpuid(struct avm_vcpu *vcpu, uint8_t *v) {
  /* A leaf the table does not have reads as zero, as KVM returns it.  The
   * lookup leaves the output alone when it finds nothing. */
  uint32_t r[4] = {0, 0, 0, 0};
  uint32_t leaf = (uint32_t)vcpu->regs.rax;
  avm_cpuid_lookup(vcpu, leaf, (uint32_t)vcpu->regs.rcx, r);
  if (leaf == 1) {
    if (VQ(v, VMCB_SAVE_CR4) & CR4_OSXSAVE)
      r[2] |= 1u << 27;
    else
      r[2] &= ~(1u << 27);
  }
  vcpu->regs.rax = r[0];
  vcpu->regs.rbx = r[1];
  vcpu->regs.rcx = r[2];
  vcpu->regs.rdx = r[3];
  vcpu->regs.rip = VQ(v, VMCB_NRIP);
  avm_step_done(vcpu);
}

/* APIC MSRs go to the in-kernel LAPIC.  Anything else injects #GP and leaves
 * RIP on the instruction. */

/* XSETBV.  A guest may only set XCR0 to the host's own value, which is what
 * the host CPU reports to the guest.  SVM cannot intercept XGETBV, so a guest
 * that could pick a different XCR0 would read back the host's value and see
 * the wrong one.  Anything else faults in the guest, and so does XSETBV
 * without OSXSAVE in CR4.  The host's XCR0 is never written. */
static void handle_xsetbv(struct avm_vcpu *vcpu, uint8_t *v) {
  if (!(VQ(v, VMCB_SAVE_CR4) & CR4_OSXSAVE)) {
    vcpu->pending_event = SVM_EVENTINJ_UD;
    return;
  }

  uint64_t val = (vcpu->regs.rdx << 32) | (vcpu->regs.rax & 0xFFFFFFFFu);
  bool valid = vcpu->regs.rcx == 0 && val == cpu_xsave_mask;
  if (!valid) {
    vcpu->pending_event = SVM_EVENTINJ_GP;
    return;
  }

  vcpu->xcr0 = val;
  vcpu->regs.rip = VQ(v, VMCB_NRIP);
  avm_step_done(vcpu);
}

bool svm_msr_in_vmcb(uint32_t msr) {
  switch (msr) {
  case MSR_EFER:
  case MSR_IA32_CR_PAT:
  case MSR_STAR:
  case MSR_LSTAR:
  case MSR_CSTAR:
  case MSR_SFMASK:
  case MSR_KERNEL_GS_BASE:
  case MSR_IA32_SYSENTER_CS:
  case MSR_IA32_SYSENTER_ESP:
  case MSR_IA32_SYSENTER_EIP:
  case MSR_FS_BASE:
  case MSR_GS_BASE:
    return true;
  default:
    return false;
  }
}

/* An address is canonical when bits 63:47 all equal bit 47. */
static bool svm_addr_canonical(uint64_t addr) {
  return (uint64_t)((int64_t)(addr << 16) >> 16) == addr;
}

/* Each PAT entry must be a memory type the CPU has: 0, 1, 4, 5, 6 or 7.  Types
 * 2 and 3 are reserved, and the bits above the type must be clear. */
static bool svm_pat_valid(uint64_t pat) {
  for (int i = 0; i < 8; i++) {
    uint8_t e = (uint8_t)(pat >> (8 * i));
    if ((e & 0xF8) || e == 2 || e == 3)
      return false;
  }
  return true;
}

int svm_msr_get(struct avm_vcpu *vcpu, uint32_t msr, uint64_t *val) {
  uint8_t *v = hhdm(vcpu->vmcb_pa);
  switch (msr) {
  case MSR_EFER:
    /* SVME is the VMM's bit for running guests, so the guest never sees it. */
    *val = VQ(v, VMCB_SAVE_EFER) & ~EFER_SVME;
    return 0;
  case MSR_IA32_CR_PAT:
    *val = VQ(v, VMCB_SAVE_G_PAT);
    return 0;
  case MSR_STAR:
    *val = VQ(v, VMCB_SAVE_STAR);
    return 0;
  case MSR_LSTAR:
    *val = VQ(v, VMCB_SAVE_LSTAR);
    return 0;
  case MSR_CSTAR:
    *val = VQ(v, VMCB_SAVE_CSTAR);
    return 0;
  case MSR_SFMASK:
    *val = VQ(v, VMCB_SAVE_SFMASK);
    return 0;
  case MSR_KERNEL_GS_BASE:
    *val = VQ(v, VMCB_SAVE_KERNEL_GS_BASE);
    return 0;
  case MSR_IA32_SYSENTER_CS:
    *val = VQ(v, VMCB_SAVE_SYSENTER_CS);
    return 0;
  case MSR_IA32_SYSENTER_ESP:
    *val = VQ(v, VMCB_SAVE_SYSENTER_ESP);
    return 0;
  case MSR_IA32_SYSENTER_EIP:
    *val = VQ(v, VMCB_SAVE_SYSENTER_EIP);
    return 0;
  case MSR_FS_BASE:
    *val = VQ(v, VMCB_SAVE_FS + 8);
    return 0;
  case MSR_GS_BASE:
    *val = VQ(v, VMCB_SAVE_GS + 8);
    return 0;
  default:
    return -EINVAL;
  }
}

/* Writes an MSR from svm_msr_in_vmcb.  Returns -EINVAL for a value the CPU
 * would refuse, which the guest sees as #GP.  The VMCB is the one copy of
 * the value, so this runs with the vCPU stopped, or from its own exit. */
int svm_msr_set(struct avm_vcpu *vcpu, uint32_t msr, uint64_t val) {
  uint8_t *v = hhdm(vcpu->vmcb_pa);
  switch (msr) {
  case MSR_EFER: {
    uint64_t cur = VQ(v, VMCB_SAVE_EFER);
    /* LMA is read-only, so the value's LMA is ignored, and so is its SVME. */
    if (val & ~(uint64_t)(EFER_WRITABLE | EFER_LMA | EFER_SVME))
      return -EINVAL;
    /* LME cannot change while paging is on, as on the CPU. */
    if (((cur ^ val) & EFER_LME) && (VQ(v, VMCB_SAVE_CR0) & CR0_PG))
      return -EINVAL;
    uint64_t next = (val & EFER_WRITABLE) | (cur & EFER_LMA) | EFER_SVME;
    VQ(v, VMCB_SAVE_EFER) = next;
    vcpu->sregs.efer = next & ~EFER_SVME;
    return 0;
  }
  case MSR_IA32_CR_PAT:
    if (!svm_pat_valid(val))
      return -EINVAL;
    VQ(v, VMCB_SAVE_G_PAT) = val;
    return 0;
  case MSR_STAR:
    VQ(v, VMCB_SAVE_STAR) = val;
    return 0;
  case MSR_LSTAR:
    if (!svm_addr_canonical(val))
      return -EINVAL;
    VQ(v, VMCB_SAVE_LSTAR) = val;
    return 0;
  case MSR_CSTAR:
    if (!svm_addr_canonical(val))
      return -EINVAL;
    VQ(v, VMCB_SAVE_CSTAR) = val;
    return 0;
  case MSR_SFMASK:
    VQ(v, VMCB_SAVE_SFMASK) = val;
    return 0;
  case MSR_KERNEL_GS_BASE:
    if (!svm_addr_canonical(val))
      return -EINVAL;
    VQ(v, VMCB_SAVE_KERNEL_GS_BASE) = val;
    return 0;
  case MSR_IA32_SYSENTER_CS:
    VQ(v, VMCB_SAVE_SYSENTER_CS) = val;
    return 0;
  case MSR_IA32_SYSENTER_ESP:
    VQ(v, VMCB_SAVE_SYSENTER_ESP) = val;
    return 0;
  case MSR_IA32_SYSENTER_EIP:
    VQ(v, VMCB_SAVE_SYSENTER_EIP) = val;
    return 0;
  case MSR_FS_BASE:
    if (!svm_addr_canonical(val))
      return -EINVAL;
    VQ(v, VMCB_SAVE_FS + 8) = val;
    vcpu->sregs.fs.base = val;
    return 0;
  case MSR_GS_BASE:
    if (!svm_addr_canonical(val))
      return -EINVAL;
    VQ(v, VMCB_SAVE_GS + 8) = val;
    vcpu->sregs.gs.base = val;
    return 0;
  default:
    return -EINVAL;
  }
}

/* The guest's RDMSR and WRMSR.  The APIC's MSRs go to the LAPIC, and the rest
 * go through avm_msr_get and avm_msr_set, which send the VMCB's MSRs to the
 * VMCB and the others to the VMM's store.  Anything neither handles is #GP.
 * The LAPIC branch comes first because avm_msr_get takes the IRQ lock for it,
 * and handle_msr already holds that lock. */
static bool guest_msr_read(struct avm_vcpu *vcpu, uint32_t msr, uint64_t *val) {
  if (avm_lapic_owns_msr(msr))
    return avm_lapic_rdmsr(vcpu, msr, val) == 0;
  return avm_msr_get(vcpu, msr, val) == 0;
}

static bool guest_msr_write(struct avm_vcpu *vcpu, uint32_t msr, uint64_t val) {
  if (avm_lapic_owns_msr(msr))
    return avm_lapic_wrmsr(vcpu, msr, val) == 0;
  return avm_msr_set(vcpu, msr, val) == 0;
}

static void handle_msr(struct avm_vcpu *vcpu, uint8_t *v) {
  bool write = VQ(v, VMCB_EXITINFO1) & 1;
  uint32_t msr = (uint32_t)vcpu->regs.rcx;
  bool ok;

  avm_irq_lock(vcpu->vm);
  if (write && msr == X2APIC_MSR_ICR) {
    /* The ICR is handled here rather than in the LAPIC, because it sends to
     * other vCPUs.  The destination is the high dword in x2APIC mode. */
    uint64_t val = (vcpu->regs.rdx << 32) | (vcpu->regs.rax & 0xFFFFFFFFu);
    avm_smp_write_icr(vcpu, (uint32_t)(val >> 32), (uint32_t)val);
    ok = true;
  } else if (write) {
    uint64_t val = (vcpu->regs.rdx << 32) | (vcpu->regs.rax & 0xFFFFFFFFu);
    ok = guest_msr_write(vcpu, msr, val);
  } else {
    uint64_t val = 0;
    ok = guest_msr_read(vcpu, msr, &val);
    if (ok) {
      vcpu->regs.rax = val & 0xFFFFFFFFu;
      vcpu->regs.rdx = val >> 32;
    }
  }
  avm_irq_unlock(vcpu->vm);

  if (ok) {
    vcpu->regs.rip = VQ(v, VMCB_NRIP);
    avm_step_done(vcpu);
    return;
  }
  vcpu->pending_event = SVM_EVENTINJ_GP;
}

/* XSAVE area size in pages, for the state the CPU currently enables.  Zero
 * when the CPU has no XSAVE, and the state is then not switched. */
static uint32_t xsave_pages_needed(void) {
  if (!cpu_has_xsave_flag)
    return 0;
  uint32_t a, b, c, d;
  svm_cpuid(0xD, 0, &a, &b, &c, &d);
  return (b + PAGE_SIZE - 1) / PAGE_SIZE;
}

static inline void xsave_to(void *area, uint64_t mask) {
  __asm__ volatile("xsave (%0)"
                   :
                   : "r"(area), "a"((uint32_t)mask), "d"((uint32_t)(mask >> 32))
                   : "memory");
}

static inline void xrstor_from(void *area, uint64_t mask) {
  __asm__ volatile("xrstor (%0)"
                   :
                   : "r"(area), "a"((uint32_t)mask), "d"((uint32_t)(mask >> 32))
                   : "memory");
}

/* The host's FPU/SIMD state goes to this CPU's area, and the guest's comes
 * in.  The reverse happens on exit.  Interrupts are off, so the host thread
 * cannot be switched while its state is out of the registers. */
/* XRSTOR faults (#GP) on a header it does not accept.  Checks the header of an
 * area before it is restored.  A bad one is logged with its physical address,
 * then rebuilt to the initial state, so the restore cannot fault. */
static void xsave_check(uint8_t *area, uint64_t pa, const char *where) {
  uint64_t comp, bv;
  uint32_t mxcsr;
  bool bad = false;

  memcpy(&comp, area + 520, sizeof(comp));
  memcpy(&bv, area + 512, sizeof(bv));
  memcpy(&mxcsr, area + 24, sizeof(mxcsr));
  if (comp || (bv & ~cpu_xsave_mask))
    bad = true;
  for (uint32_t i = 528; i < 576 && !bad; i++)
    if (area[i])
      bad = true;
  if ((bv & 2) && (mxcsr & ~0xFFBFu))
    bad = true;
  if (!bad)
    return;

  klog_puts("[AVM] bad XSAVE header (");
  klog_puts(where);
  klog_puts(") at pa=");
  klog_hex64(pa);
  klog_puts(" xstate_bv=");
  klog_hex64(bv);
  klog_puts(" xcomp_bv=");
  klog_hex64(comp);
  klog_puts(" mxcsr=");
  klog_hex64(mxcsr);
  klog_puts(" mask=");
  klog_hex64(cpu_xsave_mask);
  klog_puts("\n");

  memset(area, 0, 576);
  area[0] = 0x7F;   /* FCW = 0x037F, the reset value */
  area[1] = 0x03;
  area[24] = 0x80;  /* MXCSR = 0x1F80, the reset value */
  area[25] = 0x1F;
}

static void fpu_guest_enter(struct avm_vcpu *vcpu, uint32_t cpu) {
  if (!vcpu->xsave_pa)
    return;
  xsave_to(host_xsave_area[cpu], cpu_xsave_mask);
  xsave_check(hhdm(vcpu->xsave_pa), vcpu->xsave_pa, "guest");
  xrstor_from(hhdm(vcpu->xsave_pa), cpu_xsave_mask);
}

static void fpu_guest_exit(struct avm_vcpu *vcpu, uint32_t cpu) {
  if (!vcpu->xsave_pa)
    return;
  xsave_to(hhdm(vcpu->xsave_pa), cpu_xsave_mask);
  xsave_check(host_xsave_area[cpu], (uint64_t)host_xsave_area[cpu], "host");
  xrstor_from(host_xsave_area[cpu], cpu_xsave_mask);
}

static void internal_error(struct avm_vcpu *vcpu, uint64_t code) {
  klog_puts("[AVM] unhandled VM exit code=");
  klog_hex64(code);
  klog_puts("\n");
  vcpu->run->exit_reason = KVM_EXIT_INTERNAL_ERROR;
  vcpu->run->internal.suberror = KVM_INTERNAL_ERROR_UNEXPECTED_EXIT_REASON;
  vcpu->run->internal.ndata = 1;
  vcpu->run->internal.data[0] = code;
}

/* A signal that the run does not block ends KVM_RUN with KVM_EXIT_INTR, so the
 * VMM can handle it and enter again.  The run's mask is the one from
 * KVM_SET_SIGNAL_MASK when the VMM set one, and the thread's own mask otherwise.
 * Signals that are ignored, or that default to ignore, do not count, as in
 * thread_has_pending_signal. */
static bool run_signal_pending(const struct avm_vcpu *vcpu) {
  struct thread *t = sched_get_current();
  if (!t)
    return false;
  uint64_t blocked = vcpu->has_sigmask ? vcpu->sigmask : t->signal_mask;
  uint64_t unmaskable = (1ULL << (SIGKILL - 1)) | (1ULL << (SIGSTOP - 1));
  uint64_t pending = t->pending_signals & (~blocked | unmaskable);
  while (pending) {
    unsigned index = (unsigned)__builtin_ctzll(pending);
    pending &= pending - 1;
    const struct k_sigaction *action = &t->signal_handlers[index];
    if (action->sa_handler == (void *)SIG_IGN)
      continue;
    unsigned sig = index + 1;
    if (action->sa_handler == (void *)SIG_DFL &&
        (sig == SIGCHLD || sig == SIGURG || sig == SIGWINCH || sig == SIGCONT))
      continue;
    return true;
  }
  return false;
}

/* Reports a single-step to the VMM: the next RIP, the CS base included. */
static void report_step(struct avm_vcpu *vcpu, uint8_t *v) {
  struct kvm_run *run = vcpu->run;
  run->exit_reason = KVM_EXIT_DEBUG;
  run->debug.arch.exception = 1;
  run->debug.arch.pc = VQ(v, VMCB_SAVE_CS + 8) + vcpu->regs.rip;
  run->debug.arch.dr6 = VQ(v, VMCB_SAVE_DR6);
  run->debug.arch.dr7 = VQ(v, VMCB_SAVE_DR7);
}

/* A triple fault ends the guest, and the state that led to it is the only clue.
 * Logs the CS:RIP and the bytes there, and the control registers. */
static void klog_byte(uint8_t b) {
  static const char hex[] = "0123456789ABCDEF";
  char s[3] = {hex[b >> 4], hex[b & 15], 0};
  klog_puts(s);
}

static void report_shutdown(struct avm_vcpu *vcpu, uint8_t *v) {
  uint64_t cs_base = VQ(v, VMCB_SAVE_CS + 8);
  uint64_t rip = VQ(v, VMCB_SAVE_RIP);
  uint16_t cs_sel = *(volatile uint16_t *)(v + VMCB_SAVE_CS);
  uint8_t code[16];
  memset(code, 0xFF, sizeof(code));
  avm_mmio_guest_read(vcpu->vm, cs_base + rip, code, sizeof(code));
  klog_puts("[AVM] guest triple fault vcpu=");
  klog_uint64(vcpu->id);
  klog_puts(" cs=");
  klog_hex64(cs_sel);
  klog_puts(" rip=");
  klog_hex64(rip);
  klog_puts(" cs_base=");
  klog_hex64(cs_base);
  klog_puts("\n[AVM]   cr0=");
  klog_hex64(VQ(v, VMCB_SAVE_CR0));
  klog_puts(" cr3=");
  klog_hex64(VQ(v, VMCB_SAVE_CR3));
  klog_puts(" cr4=");
  klog_hex64(VQ(v, VMCB_SAVE_CR4));
  klog_puts(" efer=");
  klog_hex64(VQ(v, VMCB_SAVE_EFER));
  klog_puts(" rflags=");
  klog_hex64(VQ(v, VMCB_SAVE_RFLAGS));
  klog_puts("\n[AVM]   code at cs:rip:");
  for (int i = 0; i < 16; i++) {
    klog_puts(" ");
    klog_byte(code[i]);
  }
  klog_puts("\n");
}

int svm_vcpu_run(struct avm_vcpu *vcpu) {
  uint8_t *v = hhdm(vcpu->vmcb_pa);
  struct kvm_run *run = vcpu->run;

  /* immediate_exit is set by the VMM to return from KVM_RUN before any state
   * is consumed. */
  if (run->immediate_exit) {
    run->exit_reason = KVM_EXIT_INTR;
    return -EINTR;
  }
  if (vcpu->io_in_pending)
    apply_io_in(vcpu);
  if (vcpu->io_str_pending && !apply_string_in(vcpu)) {
    run->exit_reason = KVM_EXIT_INTERNAL_ERROR;
    run->internal.suberror = KVM_INTERNAL_ERROR_UNEXPECTED_EXIT_REASON;
    return 0;
  }
  avm_mmio_complete(vcpu);

  for (;;) {
    if (run_signal_pending(vcpu)) {
      run->exit_reason = KVM_EXIT_INTR;
      return -EINTR;
    }
    if (vcpu->step_exit) {
      vcpu->step_exit = false;
      report_step(vcpu, v);
      return 0;
    }
    /* An application processor waits here until its SIPI has arrived. */
    if (vcpu->mp_state != AVM_MP_RUNNABLE &&
        !avm_smp_wait_runnable(vcpu, run_signal_pending)) {
      run->exit_reason = KVM_EXIT_INTR;
      return -EINTR;
    }
    if (vcpu->state_dirty) {
      state_in(vcpu);
      vcpu->state_dirty = false;
    }
    gprs_in(vcpu);

    /* From here to the end of the entry, interrupts are off.  The vCPU is
     * marked in-guest before the interrupt check, so a kick queued by another
     * CPU either arrives as a physical interrupt or was already queued for
     * the check to see. */
    hal_irq_state_t flags = hal_irq_save();
    uint32_t cpu = cpu_get_current()->cpu_id;
    if (!cpu_ready[cpu]) {
      int err = svm_cpu_enable();
      if (err) {
        hal_irq_restore(flags);
        return err;
      }
    }
    vcpu->host_apic_id = cpu_get_current()->apic_id;
    vcpu->host_cpu = cpu;
    vcpu->last_apic_id = vcpu->host_apic_id;
    vcpu->entered = true;
    __atomic_store_n(&vcpu->in_guest, true, __ATOMIC_RELEASE);

    /* Deliver the next interrupt: the LAPIC first, then the PIC.  The guest
     * must have IF set, and an earlier event (such as #GP) goes first. */
    if (!vcpu->pending_event && (vcpu->regs.rflags & RFLAGS_IF) && !vcpu->block_irq) {
      int vector = avm_irq_next_vector(vcpu);
      if (vector >= 0)
        vcpu->pending_event = SVM_EVENTINJ_VALID | (uint64_t)vector;
    }

    /* Arm a host timer for the next guest deadline.  Its interrupt forces an
     * exit, and the loop delivers the guest timer then. */
    avm_irq_arm_timer(vcpu);

    VQ(v, VMCB_EVENTINJ) = vcpu->pending_event;
    vcpu->pending_event = 0;
    /* Flush the guest TLB on every entry.  Correct but slow; it means a NPT
     * change needs no more than a kick to take effect. */
    VB(v, VMCB_TLB_CONTROL) = 1;

    fpu_guest_enter(vcpu, cpu);
    __atomic_store_n(&vcpu->in_vmrun, true, __ATOMIC_SEQ_CST);
    svm_run_vcpu(vcpu->vmcb_pa, host_vmcb_pa[cpu], &vcpu->regs);
    __atomic_store_n(&vcpu->in_vmrun, false, __ATOMIC_SEQ_CST);
    fpu_guest_exit(vcpu, cpu);
    __atomic_store_n(&vcpu->in_guest, false, __ATOMIC_RELEASE);
    hal_irq_restore(flags);
    gprs_out(vcpu);
    uint64_t code = VQ(v, VMCB_EXITCODE);
    switch (code) {
    case SVM_EXIT_INTR:
    case SVM_EXIT_NMI:
      /* A host interrupt arrived while the guest ran.  Resume. */
      continue;
    case SVM_EXIT_CPUID:
      handle_cpuid(vcpu, v);
      continue;
    case SVM_EXIT_XSETBV:
      handle_xsetbv(vcpu, v);
      continue;
    case SVM_EXIT_MSR:
      handle_msr(vcpu, v);
      continue;
    case SVM_EXIT_HLT:
      vcpu->regs.rip = VQ(v, VMCB_NRIP);
      avm_step_done(vcpu);
      /* With an in-kernel interrupt controller the halt waits here, as KVM does:
       * a device interrupt raised later wakes it through an NMI kick.  Only a
       * signal returns it to the VMM.  Without one, the VMM handles the halt. */
      /* With IF clear nothing in the kernel can wake the CPU, so the VMM sees
       * the halt, as it did before.  With IF set, the halt waits here. */
      if (vcpu->vm->irqchip && (vcpu->regs.rflags & RFLAGS_IF)) {
        bool woke = avm_irq_wait(vcpu, true, run_signal_pending);
        if (woke)
          continue;
        run->exit_reason = KVM_EXIT_INTR;
        return -EINTR;
      }
      run->exit_reason = KVM_EXIT_HLT;
      return 0;
    case SVM_EXIT_IOIO: {
      uint64_t info = VQ(v, VMCB_EXITINFO1);
      if (try_ioeventfd_pio(vcpu, v) || try_inkernel_io(vcpu, v))
        continue;
      if (info & IOIO_STRING) {
        int r = fill_string_io(vcpu, v, info);
        if (r == AVM_STR_IN_KERNEL)
          continue;
        if (r == AVM_STR_ERROR) {
          run->exit_reason = KVM_EXIT_INTERNAL_ERROR;
          run->internal.suberror = KVM_INTERNAL_ERROR_UNEXPECTED_EXIT_REASON;
        }
        return 0;
      }
      fill_io_exit(vcpu, v);
      return 0;
    }
    case SVM_EXIT_SHUTDOWN:
      report_shutdown(vcpu, v);
      run->exit_reason = KVM_EXIT_SHUTDOWN;
      return 0;
    case SVM_EXIT_EXCP_DB:
      /* A single-step trap, after the instruction, so RIP is the next one. */
      report_step(vcpu, v);
      return 0;
    case SVM_EXIT_NPF:
      /* A memory access outside every memslot: a device register, or memory
       * the VMM answers.  The kernel models the APIC and IOAPIC. */
      switch (avm_mmio_fault(vcpu, VQ(v, VMCB_EXITINFO2))) {
      case AVM_MMIO_HANDLED:
        continue;
      case AVM_MMIO_TO_VMM:
        return 0;
      default:
        klog_puts("[AVM] undecodable MMIO access at gpa=");
        klog_hex64(VQ(v, VMCB_EXITINFO2));
        klog_puts("\n");
        internal_error(vcpu, code);
        return 0;
      }
    default:
      /* Exceptions and VMEXIT_INVALID are not handled yet.  The VMM sees the
       * raw exit code. */
      internal_error(vcpu, code);
      return 0;
    }
  }
}
