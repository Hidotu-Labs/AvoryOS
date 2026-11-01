#include "avm_irq.h"
#include "avm_irqroute.h"

#include "avm_ioapic.h"
#include "avm_lapic.h"

#include "../apic/lapic_timer.h"
#include "../hal/hal.h"
#include "../include/linux/errno.h"
#include "../lib/string.h"
#include "../lock/spinlock.h"
#include "../smp/cpu.h"

#define PIC_MASTER_CMD 0x20
#define PIC_MASTER_DATA 0x21
#define PIC_SLAVE_CMD 0xA0
#define PIC_SLAVE_DATA 0xA1
#define PIC_CASCADE_IRQ 2

#define PIT_CH0 0x40
#define PIT_CONTROL 0x43
#define PIT_HZ 1193182ull

static uint64_t now_ns(void) { return lapic_timer_get_ns(); }

static int lowest_set(uint8_t bits) {
  for (int i = 0; i < 8; i++)
    if (bits & (1u << i))
      return i;
  return -1;
}

/* ---- 8259 PIC ---------------------------------------------------------- */

/* The request the chip would deliver next, or -1.  A request is blocked by
 * any in-service interrupt of higher priority (lower IRQ number). */
static int pic_candidate(const struct avm_pic *p) {
  uint8_t req = p->irr & ~p->imr;
  if (!req)
    return -1;
  if (p->isr) {
    int h = lowest_set(p->isr);
    req &= (uint8_t)((1u << h) - 1);
  }
  return req ? lowest_set(req) : -1;
}

/* The slave's output feeds master IRQ 2, so raise or drop that request to
 * match the slave's state. */
static void pic_sync_cascade(struct avm_vm *vm) {
  struct avm_pic *m = &vm->pic[0];
  struct avm_pic *s = &vm->pic[1];
  if (pic_candidate(s) >= 0)
    m->irr |= 1u << PIC_CASCADE_IRQ;
  else
    m->irr &= ~(1u << PIC_CASCADE_IRQ);
}

static void pic_raise(struct avm_vm *vm, int irq) {
  vm->pic[irq >> 3].irr |= 1u << (irq & 7);
}

/* Acknowledges the highest-priority request and returns its vector, or -1. */
static int pic_next(struct avm_vm *vm) {
  struct avm_pic *m = &vm->pic[0];
  struct avm_pic *s = &vm->pic[1];
  pic_sync_cascade(vm);

  int mc = pic_candidate(m);
  if (mc < 0)
    return -1;

  if (mc == PIC_CASCADE_IRQ) {
    int sc = pic_candidate(s);
    m->irr &= ~(1u << PIC_CASCADE_IRQ);
    if (sc < 0)
      return -1;
    s->irr &= ~(1u << sc);
    s->isr |= 1u << sc;
    m->isr |= 1u << PIC_CASCADE_IRQ;
    return s->vector_base + sc;
  }

  m->irr &= ~(1u << mc);
  m->isr |= 1u << mc;
  return m->vector_base + mc;
}

/* Clears an in-service bit.  Once the slave has nothing in service, its
 * cascade request in the master is cleared too. */
static void pic_end_of_interrupt(struct avm_vm *vm, int chip, int irq) {
  struct avm_pic *p = &vm->pic[chip];
  if (irq < 0)
    return;
  p->isr &= ~(1u << irq);
  if (chip == 1 && p->isr == 0)
    vm->pic[0].isr &= ~(1u << PIC_CASCADE_IRQ);
}

static void pic_write_command(struct avm_vm *vm, int chip, uint8_t val) {
  struct avm_pic *p = &vm->pic[chip];

  if (val & 0x10) {
    /* ICW1: start initialization.  This clears the masks and requests. */
    p->irr = 0;
    p->isr = 0;
    p->imr = 0;
    p->init_cascade = !(val & 0x02);
    p->init_icw4 = val & 0x01;
    p->init_step = 1;
    p->read_isr = false;
    return;
  }
  if (val & 0x08) {
    /* OCW3: select what the next command read returns. */
    if (val & 0x02)
      p->read_isr = (val & 0x01) != 0;
    return;
  }

  /* OCW2: end of interrupt. */
  switch (val & 0xE0) {
  case 0x20:
    pic_end_of_interrupt(vm, chip, lowest_set(p->isr));
    break;
  case 0x60:
    pic_end_of_interrupt(vm, chip, val & 7);
    break;
  default:
    break;
  }
}

static void pic_write_data(struct avm_vm *vm, int chip, uint8_t val) {
  struct avm_pic *p = &vm->pic[chip];
  if (!p->init_step) {
    p->imr = val;
    return;
  }

  /* Initialization: ICW2 gives the vector base, then ICW3 (cascade only),
   * then ICW4 (only when ICW1 asked for it). */
  switch (p->init_step) {
  case 1:
    p->vector_base = val & 0xF8;
    p->init_step = p->init_cascade ? 2 : (p->init_icw4 ? 3 : 0);
    break;
  case 2:
    p->init_step = p->init_icw4 ? 3 : 0;
    break;
  case 3:
    p->init_step = 0;
    break;
  default:
    break;
  }
}

static uint8_t pic_read(struct avm_vm *vm, int chip, bool data) {
  struct avm_pic *p = &vm->pic[chip];
  if (data)
    return p->imr;
  return p->read_isr ? p->isr : p->irr;
}

/* ---- PIT channel 0 ----------------------------------------------------- */

static uint64_t pit_span(const struct avm_pit *p) {
  return p->reload ? p->reload : 65536;
}

/* Input clock cycles since the counter was loaded. */
static uint64_t pit_cycles(const struct avm_pit *p, uint64_t now) {
  return ((now - p->start_ns) / 1000) * PIT_HZ / 1000000;
}

static uint16_t pit_count(const struct avm_pit *p, uint64_t now) {
  uint64_t span = pit_span(p);
  if (p->period_ns == 0)
    return p->reload;  /* never loaded */

  uint64_t cycles = pit_cycles(p, now);
  if (p->periodic)
    return (uint16_t)(span - cycles % span);
  if (cycles >= span)
    return 0;
  return (uint16_t)(span - cycles);
}

static void pit_load(struct avm_pit *p, uint64_t now) {
  p->start_ns = now;
  p->period_ns = pit_span(p) * 1000000000ull / PIT_HZ;
  p->next_irq_ns = now + p->period_ns;
  p->armed = true;
}

static void pit_control(struct avm_pit *p, uint8_t val, uint64_t now) {
  if ((val >> 6) != 0)
    return;  /* channels 1 and 2 are not modelled */

  uint8_t access = (val >> 4) & 3;
  if (access == 0) {
    /* Counter latch: snapshot the count for the next read. */
    p->latch = pit_count(p, now);
    p->latched = true;
    p->read_phase = 0;
    return;
  }

  uint8_t mode = (val >> 1) & 7;
  if (mode == 6)
    mode = 2;
  else if (mode == 7)
    mode = 3;
  else if (mode != 2 && mode != 3)
    mode = 0;

  p->access = access;
  p->mode = mode;
  p->periodic = (mode == 2 || mode == 3);
  p->write_phase = 0;
  p->read_phase = 0;
  p->armed = false;
  p->period_ns = 0;
}

static void pit_write(struct avm_pit *p, uint8_t val, uint64_t now) {
  switch (p->access) {
  case 1:
    p->reload = (uint16_t)((p->reload & 0xFF00) | val);
    pit_load(p, now);
    break;
  case 2:
    p->reload = (uint16_t)((p->reload & 0x00FF) | (val << 8));
    pit_load(p, now);
    break;
  case 3:
    if (p->write_phase == 0) {
      p->write_lo = val;
      p->write_phase = 1;
    } else {
      p->reload = (uint16_t)((val << 8) | p->write_lo);
      p->write_phase = 0;
      pit_load(p, now);
    }
    break;
  default:
    break;
  }
}

static uint8_t pit_read(struct avm_pit *p, uint64_t now) {
  uint16_t src = p->latched ? p->latch : pit_count(p, now);
  uint8_t out = 0;

  switch (p->access) {
  case 1:
    out = (uint8_t)src;
    p->latched = false;
    break;
  case 2:
    out = (uint8_t)(src >> 8);
    p->latched = false;
    break;
  case 3:
    if (p->read_phase == 0) {
      out = (uint8_t)src;
      p->read_phase = 1;
    } else {
      out = (uint8_t)(src >> 8);
      p->read_phase = 0;
      p->latched = false;
    }
    break;
  default:
    break;
  }
  return out;
}

/* Raises IRQ 0 when the counter reaches its terminal count. */
static void pit_tick(struct avm_vm *vm, uint64_t now) {
  struct avm_pit *p = &vm->pit;
  if (!p->armed || now < p->next_irq_ns)
    return;

  pic_raise(vm, 0);
  if (p->periodic) {
    p->next_irq_ns += p->period_ns;
    if (p->next_irq_ns <= now)
      p->next_irq_ns = now + p->period_ns;
  } else {
    p->armed = false;
  }
}

/* ---- state for the VMM (KVM_GET/SET_IRQCHIP, KVM_GET/SET_PIT2) --------- */

#define IOAPIC_BASE_GPA 0xFEC00000ull

static void pic_export(const struct avm_pic *p, struct kvm_pic_state *s) {
  s->irr = p->irr;
  s->imr = p->imr;
  s->isr = p->isr;
  s->irq_base = p->vector_base;
  s->read_reg_select = p->read_isr ? 1 : 0;
  s->init_state = p->init_step;
  s->init4 = p->init_icw4;
}

static void pic_import(struct avm_pic *p, const struct kvm_pic_state *s) {
  p->irr = s->irr;
  p->imr = s->imr;
  p->isr = s->isr;
  p->vector_base = s->irq_base;
  p->read_isr = s->read_reg_select & 1;
  p->init_step = s->init_state;
  p->init_icw4 = s->init4;
}

static void ioapic_export(const struct avm_vm *vm, struct kvm_ioapic_state *s) {
  s->base_address = IOAPIC_BASE_GPA;
  s->ioregsel = vm->ioapic.index;
  s->id = 0;
  s->irr = 0; /* remote IRR is not modelled */
  memcpy(s->redirtbl, vm->ioapic.redir, sizeof(s->redirtbl));
}

static void ioapic_import(struct avm_vm *vm, const struct kvm_ioapic_state *s) {
  vm->ioapic.index = s->ioregsel;
  memcpy(vm->ioapic.redir, s->redirtbl, sizeof(vm->ioapic.redir));
}

int avm_irq_get_chip(struct avm_vm *vm, struct kvm_irqchip *c) {
  if (!vm->irqchip)
    return -ENXIO;
  uint32_t id = c->chip_id;
  if (id > KVM_IRQCHIP_IOAPIC)
    return -EINVAL;
  memset(&c->chip, 0, sizeof(c->chip));
  spinlock_acquire(&vm->irq_lock);
  if (id == KVM_IRQCHIP_IOAPIC)
    ioapic_export(vm, &c->chip.ioapic);
  else
    pic_export(&vm->pic[id], &c->chip.pic);
  spinlock_release(&vm->irq_lock);
  return 0;
}

int avm_irq_set_chip(struct avm_vm *vm, const struct kvm_irqchip *c) {
  if (!vm->irqchip)
    return -ENXIO;
  uint32_t id = c->chip_id;
  if (id > KVM_IRQCHIP_IOAPIC)
    return -EINVAL;
  spinlock_acquire(&vm->irq_lock);
  if (id == KVM_IRQCHIP_IOAPIC)
    ioapic_import(vm, &c->chip.ioapic);
  else
    pic_import(&vm->pic[id], &c->chip.pic);
  spinlock_release(&vm->irq_lock);
  return 0;
}

/* Channel 0 is the only PIT channel modelled.  Channels 1 and 2 read as zero
 * and ignore writes. */
static void pit_export(struct avm_pit *p, uint64_t now,
                       struct kvm_pit_channel_state *c) {
  c->count = pit_count(p, now);
  c->latched_count = p->latch;
  c->count_latched = p->latched ? 1 : 0;
  c->read_state = p->read_phase;
  c->write_state = p->write_phase;
  c->write_latch = p->write_lo;
  c->rw_mode = p->access;
  c->mode = p->mode;
  c->gate = 1;
  c->count_load_time = (int64_t)p->start_ns;
}

static void pit_import(struct avm_pit *p, uint64_t now,
                       const struct kvm_pit_channel_state *c) {
  p->access = c->rw_mode ? c->rw_mode : 3;
  p->mode = (c->mode == 2 || c->mode == 3) ? c->mode : 0;
  p->periodic = p->mode == 2 || p->mode == 3;
  p->reload = (uint16_t)c->count; /* 65536 is stored as 0 */
  p->write_phase = c->write_state & 1;
  p->read_phase = c->read_state & 1;
  p->write_lo = c->write_latch;
  p->latch = c->latched_count;
  p->latched = c->count_latched != 0;
  if (c->count || c->count_load_time) {
    pit_load(p, now);
  } else {
    /* Never loaded: the counter does not run, and IRQ 0 does not fire. */
    p->armed = false;
    p->period_ns = 0;
  }
}

int avm_irq_get_pit2(struct avm_vm *vm, struct kvm_pit_state2 *s) {
  if (!vm->pit.created)
    return -ENXIO;
  memset(s, 0, sizeof(*s));
  spinlock_acquire(&vm->irq_lock);
  pit_export(&vm->pit, now_ns(), &s->channels[0]);
  spinlock_release(&vm->irq_lock);
  return 0;
}

int avm_irq_set_pit2(struct avm_vm *vm, const struct kvm_pit_state2 *s) {
  if (!vm->pit.created)
    return -ENXIO;
  if (s->flags & ~(KVM_PIT_FLAGS_HPET_LEGACY | KVM_PIT_FLAGS_SPEAKER_DATA_ON))
    return -EINVAL;
  if (s->channels[0].rw_mode > 3)
    return -EINVAL;
  spinlock_acquire(&vm->irq_lock);
  pit_import(&vm->pit, now_ns(), &s->channels[0]);
  spinlock_release(&vm->irq_lock);
  return 0;
}

/* ---- public interface -------------------------------------------------- */

void avm_irq_vm_init(struct avm_vm *vm) {
  spinlock_init(&vm->irq_lock);
  memset(vm->pic, 0, sizeof(vm->pic));
  /* Power-on defaults: every IRQ masked, master base 0x08, slave base 0x70. */
  vm->pic[0].imr = 0xFF;
  vm->pic[0].vector_base = 0x08;
  vm->pic[1].imr = 0xFF;
  vm->pic[1].vector_base = 0x70;
  memset(&vm->pit, 0, sizeof(vm->pit));
  avm_ioapic_init(vm);
}

int avm_irq_create_pit(struct avm_vm *vm) {
  if (!vm->irqchip)
    return -EINVAL;
  spinlock_acquire(&vm->irq_lock);
  bool already = vm->pit.created;
  vm->pit.created = true;
  spinlock_release(&vm->irq_lock);
  return already ? -EEXIST : 0;
}

/* IRQ 0-15 drive the PIC.  IRQ 16-23 drive IOAPIC pins 16-23, so a pin's
 * number is its IRQ number. */
void avm_irq_line_locked(struct avm_vm *vm, uint32_t irq, bool level) {
  if (irq >= 16) {
    avm_ioapic_set_pin(vm, irq, level);
    return;
  }
  struct avm_pic *p = &vm->pic[irq >> 3];
  uint8_t bit = 1u << (irq & 7);
  if (level) {
    /* Rising edge latches a request. */
    if (!(p->line & bit))
      p->irr |= bit;
    p->line |= bit;
  } else {
    p->line &= ~bit;
  }
}

int avm_irq_set_line(struct avm_vm *vm, uint32_t irq, uint32_t level) {
  if (!vm->irqchip)
    return -ENXIO;
  if (irq >= 24)
    return -EINVAL;
  /* Goes through the GSI routing table, which can redirect the line. */
  avm_irq_gsi_set(vm, irq, level != 0);
  return 0;
}

int avm_irq_next_vector(struct avm_vcpu *vcpu) {
  struct avm_vm *vm = vcpu->vm;

  spinlock_acquire(&vm->irq_lock);
  pit_tick(vm, now_ns());
  int v = avm_lapic_next_vector(vcpu);
  if (v < 0 && vm->irqchip)
    v = pic_next(vm);
  spinlock_release(&vm->irq_lock);
  return v;
}

void avm_irq_lock(struct avm_vm *vm) { spinlock_acquire(&vm->irq_lock); }

void avm_irq_unlock(struct avm_vm *vm) { spinlock_release(&vm->irq_lock); }

/* The nearest guest deadline in host nanoseconds, or 0 when nothing can fire.
 * The caller holds the lock. */
static uint64_t next_deadline_locked(struct avm_vcpu *vcpu) {
  struct avm_vm *vm = vcpu->vm;
  uint64_t dl = 0;
  if (avm_lapic_can_wake(vcpu))
    dl = vcpu->lapic.timer_deadline_ns;
  if (vm->pit.armed && (!dl || vm->pit.next_irq_ns < dl))
    dl = vm->pit.next_irq_ns;
  return dl;
}

/* Host timers count whole milliseconds.  The deadline is rounded up, so the
 * timer never fires early.  rearm_if_earlier leaves a nearer host deadline in
 * place; the host clears its record when it fires, so the next arm succeeds. */
static void arm_host_timer(uint64_t deadline_ns) {
  lapic_timer_rearm_if_earlier((deadline_ns + 999999) / 1000000);
}

void avm_irq_arm_timer(struct avm_vcpu *vcpu) {
  struct avm_vm *vm = vcpu->vm;
  spinlock_acquire(&vm->irq_lock);
  uint64_t dl = next_deadline_locked(vcpu);
  spinlock_release(&vm->irq_lock);
  /* TEMPORARY: with no guest deadline, a 10 ms host timer still forces an
   * exit now and then, so a spinning guest shows up in the exit summary.
   * Remove once the stall is found. */
  if (!dl)
    dl = now_ns() + 10000000ull;
  arm_host_timer(dl);
}

bool avm_irq_wait(struct avm_vcpu *vcpu, bool if_set,
                  bool (*interrupted)(const struct avm_vcpu *vcpu)) {
  struct avm_vm *vm = vcpu->vm;

  /* A halted vCPU counts as in guest mode, so an interrupt raised from another
   * CPU sends it an NMI.  The NMI wakes the hlt below, and the loop checks
   * again.  The NMI does not depend on IF, so a halt with IF clear wakes too. */
  vcpu->host_cpu = cpu_get_current()->cpu_id;
  vcpu->host_apic_id = cpu_get_current()->apic_id;
  __atomic_store_n(&vcpu->in_guest, true, __ATOMIC_RELEASE);

  for (;;) {
    /* Interrupts stay off from the check to the halt.  A timer that fires in
     * between is then taken after the halt, so the wait cannot miss it. */
    hal_irq_state_t flags = hal_irq_save();
    bool ready = false;
    uint64_t dl = 0;
    spinlock_acquire(&vm->irq_lock);
    avm_lapic_tick(vcpu);
    pit_tick(vm, now_ns());
    pic_sync_cascade(vm);
    /* With IF clear the CPU takes no maskable interrupt and no timer, so only
     * a signal or an NMI kick ends the halt. */
    if (if_set) {
      ready = (vm->irqchip && pic_candidate(&vm->pic[0]) >= 0) ||
              avm_lapic_deliverable_now(vcpu);
      dl = next_deadline_locked(vcpu);
    }
    spinlock_release(&vm->irq_lock);

    if (ready || interrupted(vcpu)) {
      __atomic_store_n(&vcpu->in_guest, false, __ATOMIC_RELEASE);
      hal_irq_restore(flags);
      return ready;
    }
    if (dl)
      arm_host_timer(dl);
    /* sti;hlt: an interrupt that arrives here is taken right after the hlt,
     * so the halt returns at once. */
    __asm__ volatile("sti; hlt; cli" ::: "memory");
    hal_irq_restore(flags);
  }
}

bool avm_irq_io(struct avm_vm *vm, uint16_t port, bool in, uint8_t *value) {
  switch (port) {
  case PIC_MASTER_CMD:
  case PIC_MASTER_DATA:
  case PIC_SLAVE_CMD:
  case PIC_SLAVE_DATA: {
    if (!vm->irqchip)
      return false;
    int chip = port >= PIC_SLAVE_CMD ? 1 : 0;
    bool data = (port & 1) != 0;
    spinlock_acquire(&vm->irq_lock);
    if (in)
      *value = pic_read(vm, chip, data);
    else if (data)
      pic_write_data(vm, chip, *value);
    else
      pic_write_command(vm, chip, *value);
    spinlock_release(&vm->irq_lock);
    return true;
  }
  case PIT_CONTROL:
  case PIT_CH0: {
    if (!vm->pit.created)
      return false;
    spinlock_acquire(&vm->irq_lock);
    if (port == PIT_CONTROL) {
      if (!in)
        pit_control(&vm->pit, *value, now_ns());
      else
        *value = 0xFF;
    } else if (in) {
      *value = pit_read(&vm->pit, now_ns());
    } else {
      pit_write(&vm->pit, *value, now_ns());
    }
    spinlock_release(&vm->irq_lock);
    return true;
  }
  default:
    return false;
  }
}
