/* GSI routing, irqfd and MSI.  See avm_irqroute.h for the model. */

#include "avm_irqroute.h"

#include "avm_ioapic.h"
#include "avm_ioeventfd.h"
#include "avm_irq.h"
#include "avm_lapic.h"
#include "avm_smp.h"

#include "../include/linux/errno.h"

/* An MSI address is in the APIC window (0xFEE00000) and names the physical
 * destination APIC ID in bits 19:12. */
#define MSI_ADDR_WINDOW 0xFEEu

static uint64_t msi_addr(uint32_t lo, uint32_t hi) {
  return ((uint64_t)hi << 32) | lo;
}

/* Delivers a fixed interrupt to the APIC named by the address.  Only fixed
 * delivery to a physical destination is modelled.  The caller holds irq_lock. */
static void msi_deliver_locked(struct avm_vm *vm, uint64_t addr, uint32_t data) {
  if ((data >> 8) & 7)
    return;
  uint8_t dest = (uint8_t)(addr >> 12);
  uint8_t vector = (uint8_t)data;
  for (uint32_t i = 0; i < AVM_MAX_VCPUS; i++) {
    struct avm_vcpu *vcpu = vm->vcpus[i];
    if (vcpu && (dest == 0xFF || dest == (uint8_t)vcpu->id))
      avm_lapic_raise(vcpu, vector);
  }
}

/* Applies every routing entry for gsi.  A GSI with no entry is a legacy line
 * when it is below 24.  The caller holds irq_lock. */
static void gsi_locked(struct avm_vm *vm, uint32_t gsi, bool level) {
  bool routed = false;
  for (uint32_t i = 0; i < vm->nroutes; i++) {
    const struct kvm_irq_routing_entry *r = &vm->routes[i];
    if (r->gsi != gsi)
      continue;
    routed = true;
    if (r->type == KVM_IRQ_ROUTING_IRQCHIP) {
      uint32_t chip = r->u.irqchip.irqchip;
      uint32_t pin = r->u.irqchip.pin;
      if (chip == KVM_IRQCHIP_IOAPIC)
        avm_ioapic_set_pin(vm, pin, level);
      else
        avm_irq_line_locked(vm, (chip == KVM_IRQCHIP_PIC_SLAVE ? 8 : 0) + pin, level);
    } else if (level) {
      /* An MSI is delivered on the rising edge only. */
      msi_deliver_locked(vm, msi_addr(r->u.msi.address_lo, r->u.msi.address_hi),
                         r->u.msi.data);
    }
  }
  if (!routed && gsi < 24)
    avm_irq_line_locked(vm, gsi, level);
}

static bool gsi_routed_locked(const struct avm_vm *vm, uint32_t gsi) {
  for (uint32_t i = 0; i < vm->nroutes; i++)
    if (vm->routes[i].gsi == gsi)
      return true;
  return false;
}

int avm_irq_set_routing(struct avm_vm *vm, const struct kvm_irq_routing_entry *e,
                        uint32_t n) {
  if (!vm->irqchip)
    return -ENXIO;
  if (n > AVM_MAX_ROUTES)
    return -EINVAL;
  for (uint32_t i = 0; i < n; i++) {
    if (e[i].gsi >= AVM_MAX_GSI || e[i].flags)
      return -EINVAL;
    if (e[i].type == KVM_IRQ_ROUTING_IRQCHIP) {
      uint32_t chip = e[i].u.irqchip.irqchip;
      uint32_t pin = e[i].u.irqchip.pin;
      if (chip > KVM_IRQCHIP_IOAPIC)
        return -EINVAL;
      if (chip != KVM_IRQCHIP_IOAPIC && pin >= 8)
        return -EINVAL;
      if (chip == KVM_IRQCHIP_IOAPIC && pin >= 24)
        return -EINVAL;
    } else if (e[i].type != KVM_IRQ_ROUTING_MSI) {
      return -EINVAL;
    }
  }

  spinlock_acquire(&vm->irq_lock);
  for (uint32_t i = 0; i < n; i++)
    vm->routes[i] = e[i];
  vm->nroutes = n;
  spinlock_release(&vm->irq_lock);
  return 0;
}

void avm_irq_gsi_set(struct avm_vm *vm, uint32_t gsi, bool level) {
  spinlock_acquire(&vm->irq_lock);
  gsi_locked(vm, gsi, level);
  spinlock_release(&vm->irq_lock);
  if (level)
    avm_smp_kick_all(vm);
}

int avm_irq_signal_msi(struct avm_vm *vm, const struct kvm_msi *m) {
  if (!vm->irqchip)
    return -ENXIO;
  if (m->flags)
    return -EINVAL;
  uint64_t addr = msi_addr(m->address_lo, m->address_hi);
  if ((addr >> 20) != MSI_ADDR_WINDOW)
    return -EINVAL;

  spinlock_acquire(&vm->irq_lock);
  msi_deliver_locked(vm, addr, m->data);
  spinlock_release(&vm->irq_lock);
  avm_smp_kick_all(vm);
  return 1;
}

/* Runs when the irqfd's eventfd is signalled.  It is not called with the
 * eventfd lock held.  The entry is checked under irq_lock, so a deassign that
 * has cleared it is never acted on. */
static void irqfd_fire(void *arg) {
  struct avm_irqfd *e = arg;
  struct avm_vm *vm = e->vm;

  spinlock_acquire(&vm->irq_lock);
  bool live = e->used;
  if (live) {
    gsi_locked(vm, e->gsi, true);
    gsi_locked(vm, e->gsi, false);
  }
  spinlock_release(&vm->irq_lock);
  if (live)
    avm_smp_kick_all(vm);
}

int avm_irqfd_assign(struct avm_vm *vm, const struct kvm_irqfd *a) {
  if (!vm->irqchip)
    return -ENXIO;
  /* Resample is not supported: its EOI handshake needs a second eventfd. */
  if (a->flags & KVM_IRQFD_FLAG_RESAMPLE || a->resamplefd)
    return -EINVAL;
  if (a->gsi >= AVM_MAX_GSI)
    return -EINVAL;

  void *node;
  int err = asc_eventfd_get(a->fd, &node);
  if (err)
    return err;

  spinlock_acquire(&vm->irq_lock);
  struct avm_irqfd *slot = NULL;
  if (!gsi_routed_locked(vm, a->gsi) && a->gsi >= 24)
    err = -EINVAL;
  for (uint32_t i = 0; !err && i < AVM_MAX_IRQFDS; i++) {
    struct avm_irqfd *e = &vm->irqfds[i];
    if (!e->used) {
      if (!slot)
        slot = e;
    } else if (e->efd == node && e->gsi == a->gsi) {
      err = -EEXIST;
    }
  }
  if (!err && !slot)
    err = -ENOSPC;
  if (!err) {
    slot->used = true;
    slot->gsi = a->gsi;
    slot->efd = node;
    slot->vm = vm;
  }
  spinlock_release(&vm->irq_lock);
  if (err) {
    asc_eventfd_put(node);
    return err;
  }

  err = asc_eventfd_set_notify(node, irqfd_fire, slot);
  if (err) {
    spinlock_acquire(&vm->irq_lock);
    slot->used = false;
    slot->efd = NULL;
    spinlock_release(&vm->irq_lock);
    asc_eventfd_put(node);
  }
  return err;
}

int avm_irqfd_deassign(struct avm_vm *vm, const struct kvm_irqfd *a) {
  void *node;
  int err = asc_eventfd_get(a->fd, &node);
  if (err)
    return err;

  void *held = NULL;
  spinlock_acquire(&vm->irq_lock);
  for (uint32_t i = 0; i < AVM_MAX_IRQFDS; i++) {
    struct avm_irqfd *e = &vm->irqfds[i];
    if (e->used && e->efd == node && e->gsi == a->gsi) {
      e->used = false;
      held = e->efd;
      e->efd = NULL;
      break;
    }
  }
  spinlock_release(&vm->irq_lock);

  asc_eventfd_put(node);
  if (!held)
    return -ENOENT;
  asc_eventfd_clear_notify(held);
  asc_eventfd_put(held);
  return 0;
}

void avm_irqfd_destroy(struct avm_vm *vm) {
  for (uint32_t i = 0; i < AVM_MAX_IRQFDS; i++) {
    struct avm_irqfd *e = &vm->irqfds[i];
    spinlock_acquire(&vm->irq_lock);
    bool was_used = e->used;
    void *held = e->efd;
    e->used = false;
    e->efd = NULL;
    spinlock_release(&vm->irq_lock);
    if (was_used) {
      asc_eventfd_clear_notify(held);
      asc_eventfd_put(held);
    }
  }
}
