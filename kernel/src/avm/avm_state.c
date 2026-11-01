/* vCPU state ioctls.  The VMM reads and writes a vCPU's state through these.
 *
 * A guest's general registers are in vcpu->regs, which every exit updates.
 * Its special registers are read from the VMCB (svm_sregs_out) unless a
 * KVM_SET_SREGS is still waiting to be written there.  The FPU, SSE and XSAVE
 * state is in the vCPU's XSAVE area, and its legacy part has the FXSAVE layout. */

#include "avm_state.h"

#include "avm_irq.h"
#include "avm_lapic.h"
#include "avm_smp.h"
#include "svm.h"

#include "../apic/lapic_timer.h"
#include "../cpu/features.h"
#include "../cpu/tsc.h"
#include "../include/arch/uaccess.h"
#include "../include/linux/errno.h"
#include "../lib/string.h"
#include "../mm/pmm.h"

/* FXSAVE byte offsets inside the XSAVE area's legacy region. */
#define FX_FCW 0
#define FX_FSW 2
#define FX_FTW 4
#define FX_FOP 6
#define FX_FIP 8
#define FX_FDP 16
#define FX_MXCSR 24
#define FX_ST0 32
#define FX_XMM0 160

/* The APIC registers that KVM_GET_LAPIC and KVM_SET_LAPIC carry.  The ICR is
 * left out (sending an IPI is not a state change), as is EOI (write-only). */
#define APIC_EOI 0x0B0
#define APIC_ICR_LOW 0x300
#define APIC_ICR_HIGH 0x310
#define APIC_REG_FIRST 0x020
#define APIC_REG_LAST 0x3F0

static uint8_t *xsave_bytes(struct avm_vcpu *vcpu) {
  return (uint8_t *)(vcpu->xsave_pa + pmm_get_hhdm_offset());
}

static int get_regs(struct avm_vcpu *vcpu, uint64_t arg) {
  if (copy_to_user((void *)arg, &vcpu->regs, sizeof(vcpu->regs)))
    return -EFAULT;
  return 0;
}

static int get_sregs(struct avm_vcpu *vcpu, uint64_t arg) {
  struct kvm_sregs s;
  /* Before the first entry, or while a SET is still pending, the VMCB does
   * not hold the guest's state yet, so the VMM's last value is returned. */
  if (vcpu->state_dirty || !vcpu->entered)
    s = vcpu->sregs;
  else
    svm_sregs_out(vcpu, &s);
  if (copy_to_user((void *)arg, &s, sizeof(s)))
    return -EFAULT;
  return 0;
}

static int get_fpu(struct avm_vcpu *vcpu, uint64_t arg) {
  if (!vcpu->xsave_pa)
    return -EINVAL;
  const uint8_t *fx = xsave_bytes(vcpu);
  struct kvm_fpu f;
  memset(&f, 0, sizeof(f));
  for (int i = 0; i < 8; i++)
    memcpy(f.fpr[i], fx + FX_ST0 + 16 * i, 16);
  memcpy(&f.fcw, fx + FX_FCW, 2);
  memcpy(&f.fsw, fx + FX_FSW, 2);
  f.ftwx = fx[FX_FTW];
  memcpy(&f.last_opcode, fx + FX_FOP, 2);
  memcpy(&f.last_ip, fx + FX_FIP, 8);
  memcpy(&f.last_dp, fx + FX_FDP, 8);
  for (int i = 0; i < 16; i++)
    memcpy(f.xmm[i], fx + FX_XMM0 + 16 * i, 16);
  memcpy(&f.mxcsr, fx + FX_MXCSR, 4);
  if (copy_to_user((void *)arg, &f, sizeof(f)))
    return -EFAULT;
  return 0;
}

static int set_fpu(struct avm_vcpu *vcpu, uint64_t arg) {
  if (!vcpu->xsave_pa)
    return -EINVAL;
  struct kvm_fpu f;
  if (copy_from_user(&f, (const void *)arg, sizeof(f)))
    return -EFAULT;
  uint8_t *fx = xsave_bytes(vcpu);
  for (int i = 0; i < 8; i++)
    memcpy(fx + FX_ST0 + 16 * i, f.fpr[i], 16);
  memcpy(fx + FX_FCW, &f.fcw, 2);
  memcpy(fx + FX_FSW, &f.fsw, 2);
  fx[FX_FTW] = f.ftwx;
  memcpy(fx + FX_FOP, &f.last_opcode, 2);
  memcpy(fx + FX_FIP, &f.last_ip, 8);
  memcpy(fx + FX_FDP, &f.last_dp, 8);
  for (int i = 0; i < 16; i++)
    memcpy(fx + FX_XMM0 + 16 * i, f.xmm[i], 16);
  memcpy(fx + FX_MXCSR, &f.mxcsr, 4);
  return 0;
}

/* KVM_GET_XSAVE and KVM_SET_XSAVE carry the first 4 KB of the area.  A larger
 * host layout (for components past 4 KB) is not carried. */
static int get_xsave(struct avm_vcpu *vcpu, uint64_t arg) {
  if (!vcpu->xsave_pa)
    return -EINVAL;
  if (copy_to_user((void *)arg, xsave_bytes(vcpu), 4096))
    return -EFAULT;
  return 0;
}

/* XRSTOR faults (#GP) on a header it does not accept, and on MXCSR bits the CPU
 * reserves.  The VMM's area is cleaned up before the guest uses it, the way
 * KVM does: the standard format only, state limited to the guest's XCR0 and
 * to what the CPU has, reserved header bytes clear, and MXCSR with only the
 * defined bits (0xFFBF; the bits above 15 and DAZ are cleared). */
static void sanitize_xsave(struct avm_vcpu *vcpu, uint8_t *area) {
  uint64_t bv;
  uint32_t mxcsr;

  /* Compacted format is not used, so XCOMP_BV is cleared. */
  memset(area + 520, 0, 8);
  memcpy(&bv, area + 512, sizeof(bv));      /* XSTATE_BV */
  bv &= vcpu->xcr0 & cpu_xsave_mask;
  memcpy(area + 512, &bv, sizeof(bv));
  memset(area + 528, 0, 48);                /* reserved header bytes */
  memcpy(&mxcsr, area + 24, sizeof(mxcsr));
  mxcsr &= 0xFFBFu;
  memcpy(area + 24, &mxcsr, sizeof(mxcsr));
}

static int set_xsave(struct avm_vcpu *vcpu, uint64_t arg) {
  if (!vcpu->xsave_pa)
    return -EINVAL;
  uint8_t *area = xsave_bytes(vcpu);
  if (copy_from_user(area, (const void *)arg, 4096))
    return -EFAULT;
  sanitize_xsave(vcpu, area);
  return 0;
}

static int get_xcrs(struct avm_vcpu *vcpu, uint64_t arg) {
  struct kvm_xcrs x;
  memset(&x, 0, sizeof(x));
  x.nr_xcrs = 1;
  x.xcrs[0].xcr = 0;
  x.xcrs[0].value = vcpu->xcr0;
  if (copy_to_user((void *)arg, &x, sizeof(x)))
    return -EFAULT;
  return 0;
}

/* Only XCR0 exists.  The VMM may set any subset of the host's XCR0 that keeps
 * x87 on, which is what a reset guest has.  The guest's own XSETBV is still
 * limited to the host value (handle_xsetbv). */
static int set_xcrs(struct avm_vcpu *vcpu, uint64_t arg) {
  struct kvm_xcrs x;
  if (copy_from_user(&x, (const void *)arg, sizeof(x)))
    return -EFAULT;
  if (x.nr_xcrs > KVM_MAX_XCRS)
    return -EINVAL;
  for (uint32_t i = 0; i < x.nr_xcrs; i++) {
    if (x.xcrs[i].xcr != 0)
      return -EINVAL;
    uint64_t v = x.xcrs[i].value;
    if (!(v & 1) || (v & ~cpu_xsave_mask))
      return -EINVAL;
    vcpu->xcr0 = v;
  }
  return 0;
}

static int get_lapic(struct avm_vcpu *vcpu, uint64_t arg) {
  struct kvm_lapic_state l;
  memset(&l, 0, sizeof(l));
  avm_irq_lock(vcpu->vm);
  for (uint32_t off = APIC_REG_FIRST; off <= APIC_REG_LAST; off += 0x10) {
    uint32_t v = 0;
    if (avm_lapic_mmio_read(vcpu, off, &v) == 0)
      memcpy(l.regs + off, &v, 4);
  }
  avm_irq_unlock(vcpu->vm);
  if (copy_to_user((void *)arg, &l, sizeof(l)))
    return -EFAULT;
  return 0;
}

static int set_lapic(struct avm_vcpu *vcpu, uint64_t arg) {
  struct kvm_lapic_state l;
  if (copy_from_user(&l, (const void *)arg, sizeof(l)))
    return -EFAULT;
  avm_irq_lock(vcpu->vm);
  for (uint32_t off = APIC_REG_FIRST; off <= APIC_REG_LAST; off += 0x10) {
    if (off == APIC_ICR_LOW || off == APIC_ICR_HIGH || off == APIC_EOI)
      continue;
    uint32_t v;
    memcpy(&v, l.regs + off, 4);
    avm_lapic_mmio_write(vcpu, off, v); /* read-only registers refuse */
  }
  avm_irq_unlock(vcpu->vm);
  return 0;
}

static int get_mp_state(struct avm_vcpu *vcpu, uint64_t arg) {
  struct kvm_mp_state m;
  switch (vcpu->mp_state) {
  case AVM_MP_RUNNABLE:
    m.mp_state = KVM_MP_STATE_RUNNABLE;
    break;
  case AVM_MP_INIT_DONE:
    m.mp_state = KVM_MP_STATE_INIT_RECEIVED;
    break;
  default:
    m.mp_state = KVM_MP_STATE_UNINITIALIZED;
    break;
  }
  if (copy_to_user((void *)arg, &m, sizeof(m)))
    return -EFAULT;
  return 0;
}

static int set_mp_state(struct avm_vcpu *vcpu, uint64_t arg) {
  struct kvm_mp_state m;
  if (copy_from_user(&m, (const void *)arg, sizeof(m)))
    return -EFAULT;
  switch (m.mp_state) {
  case KVM_MP_STATE_RUNNABLE:
    vcpu->mp_state = AVM_MP_RUNNABLE;
    return 0;
  case KVM_MP_STATE_UNINITIALIZED:
    vcpu->mp_state = AVM_MP_UNINIT;
    return 0;
  case KVM_MP_STATE_INIT_RECEIVED:
    vcpu->mp_state = AVM_MP_INIT_DONE;
    return 0;
  default:
    return -EINVAL;
  }
}

/* Pending exceptions, interrupts, NMIs and triple faults are not kept, so a
 * set that asks for one is refused.  Everything else is accepted and ignored. */
static int get_events(struct avm_vcpu *vcpu, uint64_t arg) {
  (void)vcpu;
  struct kvm_vcpu_events e;
  memset(&e, 0, sizeof(e));
  if (copy_to_user((void *)arg, &e, sizeof(e)))
    return -EFAULT;
  return 0;
}

static int set_events(struct avm_vcpu *vcpu, uint64_t arg) {
  (void)vcpu;
  struct kvm_vcpu_events e;
  if (copy_from_user(&e, (const void *)arg, sizeof(e)))
    return -EFAULT;
  if (e.exception.injected || e.exception.pending || e.interrupt.injected ||
      e.nmi.injected || e.nmi.pending || e.triple_fault.pending)
    return -EINVAL;
  return 0;
}

/* Debug registers are not kept: the guest's DR6 and DR7 are the reset values,
 * and breakpoints cannot be set. */
static int get_debugregs(uint64_t arg) {
  struct kvm_debugregs d;
  memset(&d, 0, sizeof(d));
  d.dr6 = 0xFFFF0FF0ull;
  d.dr7 = 0x400;
  if (copy_to_user((void *)arg, &d, sizeof(d)))
    return -EFAULT;
  return 0;
}

static int set_debugregs(uint64_t arg) {
  struct kvm_debugregs d;
  if (copy_from_user(&d, (const void *)arg, sizeof(d)))
    return -EFAULT;
  for (int i = 0; i < 4; i++)
    if (d.db[i])
      return -EINVAL;
  if (d.dr7 & 0xFF) /* local and global breakpoint enables */
    return -EINVAL;
  return 0;
}

static int set_signal_mask(struct avm_vcpu *vcpu, uint64_t arg) {
  struct kvm_signal_mask hdr;
  if (copy_from_user(&hdr, (const void *)arg, sizeof(hdr)))
    return -EFAULT;
  if (hdr.len == 0) {
    vcpu->has_sigmask = false;
    return 0;
  }
  if (hdr.len != 8) /* the sigset is 64 bits */
    return -EINVAL;
  uint64_t set;
  if (copy_from_user(&set, (const void *)(arg + sizeof(hdr)), sizeof(set)))
    return -EFAULT;
  vcpu->sigmask = set;
  vcpu->has_sigmask = true;
  return 0;
}

/* Single-step only.  Breakpoints, and injecting a debug exception, are not
 * supported, so a request for them is refused.  BLOCKIRQ holds back interrupt
 * delivery while stepping. */
static int set_guest_debug(struct avm_vcpu *vcpu, uint64_t arg) {
  struct kvm_guest_debug d;
  if (copy_from_user(&d, (const void *)arg, sizeof(d)))
    return -EFAULT;
  if (d.control & (KVM_GUESTDBG_USE_SW_BP | KVM_GUESTDBG_USE_HW_BP |
                   KVM_GUESTDBG_INJECT_DB | KVM_GUESTDBG_INJECT_BP))
    return -EINVAL;
  vcpu->singlestep = (d.control & KVM_GUESTDBG_ENABLE) &&
                     (d.control & KVM_GUESTDBG_SINGLESTEP);
  vcpu->block_irq = vcpu->singlestep && (d.control & KVM_GUESTDBG_BLOCKIRQ);
  return 0;
}

static int setup_mce(struct avm_vcpu *vcpu, uint64_t arg) {
  uint64_t cap;
  if (copy_from_user(&cap, (const void *)arg, sizeof(cap)))
    return -EFAULT;
  /* As KVM does: the low byte is the bank count, which must be from one to
   * what we report.  Outside that byte only MCG_CTL_P is accepted.  QEMU
   * masks its MCG_CAP to what GET_MCE_CAP_SUPPORTED reports, and keeps the bank
   * count, so the bank byte must pass. */
  if ((cap & 0xff) == 0 || (cap & 0xff) > AVM_MCE_BANKS)
    return -EINVAL;
  if (cap & ~(uint64_t)(MCG_CTL_P | 0xff))
    return -EINVAL;
  vcpu->mcg_cap = cap;
  return 0;
}

int avm_vcpu_state_ioctl(struct avm_vcpu *vcpu, uint32_t request, uint64_t arg) {
  switch (request) {
  case KVM_SET_VAPIC_ADDR: {
    /* QEMU sends address 0 when no VAPIC page is set up, and that is
     * accepted.  A real page would need hardware TPR shadowing, which this
     * kernel does not provide. */
    struct kvm_vapic_addr a;
    if (copy_from_user(&a, (const void *)arg, sizeof(a)))
      return -EFAULT;
    return a.vapic_addr ? -EINVAL : 0;
  }
  case KVM_TPR_ACCESS_REPORTING: {
    /* TPR access reports are not produced.  Turning them off changes
     * nothing, and turning them on is refused. */
    struct kvm_tpr_access_ctl c;
    if (copy_from_user(&c, (const void *)arg, sizeof(c)))
      return -EFAULT;
    if (c.flags)
      return -EINVAL;
    return c.enabled ? -EINVAL : 0;
  }
  case KVM_GET_REGS:
    return get_regs(vcpu, arg);
  case KVM_GET_SREGS:
    return get_sregs(vcpu, arg);
  case KVM_GET_FPU:
    return get_fpu(vcpu, arg);
  case KVM_SET_FPU:
    return set_fpu(vcpu, arg);
  case KVM_GET_XSAVE:
    return get_xsave(vcpu, arg);
  case KVM_SET_XSAVE:
    return set_xsave(vcpu, arg);
  case KVM_GET_XCRS:
    return get_xcrs(vcpu, arg);
  case KVM_SET_XCRS:
    return set_xcrs(vcpu, arg);
  case KVM_GET_LAPIC:
    return get_lapic(vcpu, arg);
  case KVM_SET_LAPIC:
    return set_lapic(vcpu, arg);
  case KVM_GET_MP_STATE:
    return get_mp_state(vcpu, arg);
  case KVM_SET_MP_STATE:
    return set_mp_state(vcpu, arg);
  case KVM_GET_VCPU_EVENTS:
    return get_events(vcpu, arg);
  case KVM_SET_VCPU_EVENTS:
    return set_events(vcpu, arg);
  case KVM_GET_DEBUGREGS:
    return get_debugregs(arg);
  case KVM_SET_DEBUGREGS:
    return set_debugregs(arg);
  case KVM_GET_TSC_KHZ:
    return (int)tsc_get_freq_khz();
  case KVM_SET_TSC_KHZ:
    /* The guest runs at the host's TSC rate; another rate is refused. */
    return arg == tsc_get_freq_khz() ? 0 : -EINVAL;
  case KVM_SET_SIGNAL_MASK:
    return set_signal_mask(vcpu, arg);
  case KVM_SET_GUEST_DEBUG:
    return set_guest_debug(vcpu, arg);
  case KVM_X86_SETUP_MCE:
    return setup_mce(vcpu, arg);
  case KVM_X86_SET_MCE:
    /* Machine-check injection is not supported. */
    return -EINVAL;
  default:
    return -ENOTTY;
  }
}
