/* VM-level ioctls: routing, irqfd, MSI, and the clock.  The routing and irqfd
 * logic is in avm_irqroute.c. */

#include "avm_vm_misc.h"

#include "avm_irq.h"
#include "avm_irqroute.h"

#include "../apic/lapic_timer.h"
#include "../include/arch/uaccess.h"
#include "../include/linux/errno.h"
#include "../lib/string.h"
#include "../mm/heap.h"

static int set_gsi_routing(struct avm_vm *vm, uint64_t arg) {
  struct kvm_irq_routing hdr;
  if (copy_from_user(&hdr, (const void *)arg, sizeof(hdr)))
    return -EFAULT;
  if (hdr.nr > AVM_MAX_ROUTES || hdr.flags)
    return -EINVAL;

  struct kvm_irq_routing_entry *e = kmalloc(AVM_MAX_ROUTES * sizeof(*e));
  if (!e)
    return -ENOMEM;
  int err = 0;
  if (copy_from_user(e, (const void *)(arg + sizeof(hdr)), hdr.nr * sizeof(*e)))
    err = -EFAULT;
  else
    err = avm_irq_set_routing(vm, e, hdr.nr);
  kfree(e);
  return err;
}

static int irqfd_ioctl(struct avm_vm *vm, uint64_t arg) {
  struct kvm_irqfd a;
  if (copy_from_user(&a, (const void *)arg, sizeof(a)))
    return -EFAULT;
  if (a.flags & ~(KVM_IRQFD_FLAG_DEASSIGN | KVM_IRQFD_FLAG_RESAMPLE))
    return -EINVAL;
  if (a.flags & KVM_IRQFD_FLAG_DEASSIGN)
    return avm_irqfd_deassign(vm, &a);
  return avm_irqfd_assign(vm, &a);
}

static int signal_msi(struct avm_vm *vm, uint64_t arg) {
  struct kvm_msi m;
  if (copy_from_user(&m, (const void *)arg, sizeof(m)))
    return -EFAULT;
  return avm_irq_signal_msi(vm, &m);
}

/* The guest's clock is the host's monotonic time.  A VMM's clock set is
 * accepted, but it has no effect. */
static int get_clock(uint64_t arg) {
  struct kvm_clock_data c;
  memset(&c, 0, sizeof(c));
  c.clock = lapic_timer_get_ns();
  if (copy_to_user((void *)arg, &c, sizeof(c)))
    return -EFAULT;
  return 0;
}

static int set_clock(uint64_t arg) {
  struct kvm_clock_data c;
  if (copy_from_user(&c, (const void *)arg, sizeof(c)))
    return -EFAULT;
  return 0;
}

static int get_irqchip(struct avm_vm *vm, uint64_t arg) {
  struct kvm_irqchip c;
  if (copy_from_user(&c, (const void *)arg, sizeof(c)))
    return -EFAULT;
  int err = avm_irq_get_chip(vm, &c);
  if (err)
    return err;
  if (copy_to_user((void *)arg, &c, sizeof(c)))
    return -EFAULT;
  return 0;
}

static int set_irqchip(struct avm_vm *vm, uint64_t arg) {
  struct kvm_irqchip c;
  if (copy_from_user(&c, (const void *)arg, sizeof(c)))
    return -EFAULT;
  return avm_irq_set_chip(vm, &c);
}

static int get_pit2(struct avm_vm *vm, uint64_t arg) {
  struct kvm_pit_state2 s;
  int err = avm_irq_get_pit2(vm, &s);
  if (err)
    return err;
  if (copy_to_user((void *)arg, &s, sizeof(s)))
    return -EFAULT;
  return 0;
}

static int set_pit2(struct avm_vm *vm, uint64_t arg) {
  struct kvm_pit_state2 s;
  if (copy_from_user(&s, (const void *)arg, sizeof(s)))
    return -EFAULT;
  return avm_irq_set_pit2(vm, &s);
}

int avm_vm_misc_ioctl(struct avm_vm *vm, uint32_t request, uint64_t arg) {
  switch (request) {
  case KVM_GET_IRQCHIP:
    return get_irqchip(vm, arg);
  case KVM_SET_IRQCHIP:
    return set_irqchip(vm, arg);
  case KVM_GET_PIT2:
    return get_pit2(vm, arg);
  case KVM_SET_PIT2:
    return set_pit2(vm, arg);
  case KVM_SET_TSS_ADDR:
    /* Intel's real-mode TSS area. SVM does not use one. */
    return 0;
  case KVM_SET_IDENTITY_MAP_ADDR: {
    uint64_t addr;
    if (copy_from_user(&addr, (const void *)arg, sizeof(addr)))
      return -EFAULT;
    /* The identity map is an Intel EPT requirement, and SVM does not use it. */
    return 0;
  }
  case KVM_SET_GSI_ROUTING:
    return set_gsi_routing(vm, arg);
  case KVM_IRQFD:
    return irqfd_ioctl(vm, arg);
  case KVM_SIGNAL_MSI:
    return signal_msi(vm, arg);
  case KVM_GET_CLOCK:
    return get_clock(arg);
  case KVM_SET_CLOCK:
    return set_clock(arg);
  default:
    return -ENOTTY;
  }
}
