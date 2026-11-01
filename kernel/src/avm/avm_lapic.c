#include "avm_lapic.h"

#include "../apic/lapic_timer.h"
#include "../include/linux/errno.h"
#include "../lib/string.h"

#define SVR_ENABLE (1u << 8)
#define LVT_MASKED (1u << 16)
#define LVT_TIMER_PERIODIC (1u << 17)
#define LVT_TIMER_TSC_DEADLINE (1u << 18)
/* Writable LVT bits: vector 7:0, delivery mode 10:8, polarity 13,
 * trigger 15, mask 16, timer mode 17.  Bit 12 (status) and bit 14 (remote
 * IRR) are read-only. */
#define LVT_WRITABLE_MASK 0x3A7FFu

/* x2APIC MSR numbers, as offsets from 0x800 (the xAPIC offset divided by 16). */
#define X2_ID 0x802
#define X2_VERSION 0x803
#define X2_TPR 0x808
#define X2_PPR 0x80A
#define X2_EOI 0x80B
#define X2_LDR 0x80D
#define X2_SVR 0x80F
#define X2_ISR0 0x810
#define X2_TMR0 0x818
#define X2_IRR0 0x820
#define X2_ESR 0x828
#define X2_ICR 0x830
#define X2_LVT_TIMER 0x832
#define X2_LVT_ERROR 0x837
#define X2_TIMER_INIT 0x838
#define X2_TIMER_CUR 0x839
#define X2_TIMER_DIV 0x83E

static void bit_set(uint32_t *bits, int v) { bits[v >> 5] |= 1u << (v & 31); }
static void bit_clear(uint32_t *bits, int v) { bits[v >> 5] &= ~(1u << (v & 31)); }

/* Highest set bit in a 256-bit vector set, or -1. */
static int highest_bit(const uint32_t *bits) {
  for (int i = 7; i >= 0; i--) {
    if (!bits[i])
      continue;
    for (int b = 31; b >= 0; b--)
      if (bits[i] & (1u << b))
        return i * 32 + b;
  }
  return -1;
}

/* Divisor from the divide configuration register.  Encoding 0b111 means 1. */
static uint64_t timer_divisor(uint32_t div) {
  uint32_t enc = (div & 3) | ((div >> 1) & 4);
  return enc == 7 ? 1 : 2ull << enc;
}

static void timer_start(struct avm_lapic *l, uint64_t now) {
  if (!l->timer_init) {
    l->timer_armed = false;
    return;
  }
  l->timer_period_ns = (uint64_t)l->timer_init * timer_divisor(l->timer_div) * 1000;
  l->timer_deadline_ns = now + l->timer_period_ns;
  l->timer_armed = true;
}

static uint32_t timer_current(const struct avm_lapic *l, uint64_t now) {
  if (!l->timer_armed || now >= l->timer_deadline_ns)
    return 0;
  uint64_t unit_ns = timer_divisor(l->timer_div) * 1000;
  return (uint32_t)((l->timer_deadline_ns - now + unit_ns - 1) / unit_ns);
}

/* The vector for PPR: the TPR when its class is at least the in-service
 * class, otherwise the in-service class with its low nibble clear. */
static uint32_t ppr_value(const struct avm_lapic *l) {
  int isr = highest_bit(l->isr);
  if (isr < 0 || (l->tpr >> 4) >= ((uint32_t)isr >> 4))
    return l->tpr;
  return (uint32_t)isr & 0xF0;
}

static void timer_tick(struct avm_lapic *l, uint64_t now) {
  if (!l->timer_armed || now < l->timer_deadline_ns)
    return;
  if (!(l->lvt[0] & LVT_MASKED))
    bit_set(l->irr, l->lvt[0] & 0xFF);

  if ((l->lvt[0] & LVT_TIMER_PERIODIC) && l->timer_period_ns) {
    l->timer_deadline_ns += l->timer_period_ns;
    if (l->timer_deadline_ns <= now)
      l->timer_deadline_ns = now + l->timer_period_ns;
  } else {
    l->timer_armed = false;
  }
}

void avm_lapic_init(struct avm_vcpu *vcpu) {
  struct avm_lapic *l = &vcpu->lapic;
  memset(l, 0, sizeof(*l));
  /* Reset value: BSP flag and global enable.  Software enable is off. */
  l->apic_base = 0xFEE00000ull | (1ull << 11) | (vcpu->id == 0 ? (1ull << 8) : 0);
  l->svr = 0xFF;
  for (int i = 0; i < 6; i++)
    l->lvt[i] = LVT_MASKED;
  l->timer_div = 0;
}

int avm_lapic_rdmsr(struct avm_vcpu *vcpu, uint32_t msr, uint64_t *out) {
  struct avm_lapic *l = &vcpu->lapic;
  if (!vcpu->vm->irqchip)
    return -1;
  uint64_t now = lapic_timer_get_ns();

  if (msr == MSR_APIC_BASE) {
    *out = l->apic_base;
    return 0;
  }
  if (msr < X2APIC_MSR_FIRST || msr > X2APIC_MSR_LAST)
    return -1;

  switch (msr) {
  case X2_ID:
    *out = vcpu->id;
    return 0;
  case X2_VERSION:
    /* Version 0x14, six LVT entries (max LVT index 5). */
    *out = 0x00050014;
    return 0;
  case X2_TPR:
    *out = l->tpr;
    return 0;
  case X2_PPR:
    *out = ppr_value(l);
    return 0;
  case X2_LDR:
    *out = (1u << (vcpu->id & 0xF)) | ((vcpu->id & ~0xFu) << 16);
    return 0;
  case X2_SVR:
    *out = l->svr;
    return 0;
  case X2_ESR:
    *out = l->esr;
    return 0;
  case X2_LVT_TIMER:
  case X2_LVT_TIMER + 1:
  case X2_LVT_TIMER + 2:
  case X2_LVT_TIMER + 3:
  case X2_LVT_TIMER + 4:
  case X2_LVT_ERROR:
    *out = l->lvt[msr == X2_LVT_ERROR ? 5 : msr - X2_LVT_TIMER];
    return 0;
  case X2_TIMER_INIT:
    *out = l->timer_init;
    return 0;
  case X2_TIMER_CUR:
    *out = timer_current(l, now);
    return 0;
  case X2_TIMER_DIV:
    *out = l->timer_div;
    return 0;
  default:
    break;
  }

  /* ISR, TMR and IRR banks: eight 32-bit words each. */
  if (msr >= X2_ISR0 && msr < X2_ISR0 + 8) {
    *out = l->isr[msr - X2_ISR0];
    return 0;
  }
  if (msr >= X2_TMR0 && msr < X2_TMR0 + 8) {
    *out = l->tmr[msr - X2_TMR0];
    return 0;
  }
  if (msr >= X2_IRR0 && msr < X2_IRR0 + 8) {
    *out = l->irr[msr - X2_IRR0];
    return 0;
  }
  /* EOI is write-only; ICR needs the multi-vCPU path (phase 3). */
  return -1;
}

int avm_lapic_wrmsr(struct avm_vcpu *vcpu, uint32_t msr, uint64_t val) {
  struct avm_lapic *l = &vcpu->lapic;
  if (!vcpu->vm->irqchip)
    return -1;
  uint64_t now = lapic_timer_get_ns();

  if (msr == MSR_APIC_BASE) {
    l->apic_base = val;
    return 0;
  }
  if (msr < X2APIC_MSR_FIRST || msr > X2APIC_MSR_LAST)
    return -1;
  if (val >> 32)
    return -1;
  uint32_t v = (uint32_t)val;

  switch (msr) {
  case X2_TPR:
    l->tpr = v & 0xFF;
    return 0;
  case X2_EOI: {
    if (v)
      return -1;
    int isr = highest_bit(l->isr);
    if (isr >= 0)
      bit_clear(l->isr, isr);
    return 0;
  }
  case X2_SVR:
    l->svr = v & 0x3FF;
    return 0;
  case X2_ESR:
    l->esr = 0;
    return 0;
  case X2_LVT_TIMER:
    if (v & LVT_TIMER_TSC_DEADLINE)
      return -1;  /* TSC-deadline mode is not emulated. */
    l->lvt[0] = v & LVT_WRITABLE_MASK;
    return 0;
  case X2_LVT_TIMER + 1:
  case X2_LVT_TIMER + 2:
  case X2_LVT_TIMER + 3:
  case X2_LVT_TIMER + 4:
    l->lvt[msr - X2_LVT_TIMER] = v & LVT_WRITABLE_MASK;
    return 0;
  case X2_LVT_ERROR:
    l->lvt[5] = v & LVT_WRITABLE_MASK;
    return 0;
  case X2_TIMER_INIT:
    l->timer_init = v;
    timer_start(l, now);
    return 0;
  case X2_TIMER_DIV:
    l->timer_div = v & 0xB;
    return 0;
  default:
    break;
  }
  /* ID, VERSION, LDR, PPR, ISR, TMR, IRR, CUR and ICR are not writable here. */
  return -1;
}

/* The highest pending vector that TPR and the in-service set let through, or
 * -1.  Does not change any state. */
static int deliverable(const struct avm_lapic *l) {
  if (!(l->svr & SVR_ENABLE))
    return -1;
  int v = highest_bit(l->irr);
  if (v < 0)
    return -1;
  /* The vector must beat the TPR class and the in-service class. */
  if ((uint32_t)v >> 4 <= l->tpr >> 4)
    return -1;
  int isr = highest_bit(l->isr);
  if (isr >= 0 && (v >> 4) <= (isr >> 4))
    return -1;
  return v;
}

int avm_lapic_next_vector(struct avm_vcpu *vcpu) {
  struct avm_lapic *l = &vcpu->lapic;
  timer_tick(l, lapic_timer_get_ns());

  int v = deliverable(l);
  if (v < 0)
    return -1;
  bit_clear(l->irr, v);
  bit_set(l->isr, v);
  return v;
}

void avm_lapic_tick(struct avm_vcpu *vcpu) {
  timer_tick(&vcpu->lapic, lapic_timer_get_ns());
}

void avm_lapic_raise(struct avm_vcpu *vcpu, uint8_t vector) {
  if (vector < 16)
    return;
  bit_set(vcpu->lapic.irr, vector);
}

/* The x2APIC MSR for an xAPIC register offset, or false when the offset has
 * no register.  ICR is two 32-bit registers here and one MSR there, so it is
 * left out. */
static bool xapic_msr(uint32_t off, uint32_t *msr) {
  if (off < 0x20 || off > 0x3F0 || (off & 0xF))
    return false;
  if (off == 0x300 || off == 0x310)
    return false;
  *msr = X2APIC_MSR_FIRST + (off >> 4);
  return true;
}

int avm_lapic_mmio_read(struct avm_vcpu *vcpu, uint32_t off, uint32_t *out) {
  uint32_t msr;
  uint64_t v = 0;
  *out = 0;
  if (off == 0x20) {
    /* The xAPIC ID register holds the ID in bits 31:24. */
    *out = vcpu->id << 24;
    return 0;
  }
  if (!xapic_msr(off, &msr))
    return -1;
  if (avm_lapic_rdmsr(vcpu, msr, &v) != 0)
    return -1;
  *out = (uint32_t)v;
  return 0;
}

int avm_lapic_mmio_write(struct avm_vcpu *vcpu, uint32_t off, uint32_t val) {
  uint32_t msr;
  if (!xapic_msr(off, &msr))
    return -1;
  return avm_lapic_wrmsr(vcpu, msr, val);
}

bool avm_lapic_deliverable_now(const struct avm_vcpu *vcpu) {
  return deliverable(&vcpu->lapic) >= 0;
}

bool avm_lapic_can_wake(const struct avm_vcpu *vcpu) {
  const struct avm_lapic *l = &vcpu->lapic;
  return (l->svr & SVR_ENABLE) && !(l->lvt[0] & LVT_MASKED) && l->timer_armed;
}
