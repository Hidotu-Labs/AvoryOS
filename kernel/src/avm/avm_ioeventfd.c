/* ioeventfd: a guest write to a registered port or address signals an eventfd
 * and resumes the guest, so the VMM does not see the exit.  The VMM registers
 * the match (port or address, optional length and data) and waits on the
 * eventfd.  Reads never match. */

#include "avm_ioeventfd.h"

#include "../include/linux/errno.h"

/* Mask for a write of len bytes, as used for datamatch. */
static uint64_t len_mask(uint32_t len) {
  return len >= 8 ? ~0ull : (1ull << (len * 8)) - 1;
}

static bool same_match(const struct avm_ioeventfd *e, bool pio, uint64_t addr,
                       uint32_t len, bool datamatch, uint64_t match) {
  if (e->pio != pio || e->addr != addr || e->len != len)
    return false;
  if (e->datamatch != datamatch)
    return false;
  return !datamatch || e->match == match;
}

int avm_ioeventfd_assign(struct avm_vm *vm, const struct kvm_ioeventfd *a) {
  void *node;
  int err = asc_eventfd_get(a->fd, &node);
  if (err)
    return err;

  bool pio = a->flags & KVM_IOEVENTFD_FLAG_PIO;
  bool datamatch = a->flags & KVM_IOEVENTFD_FLAG_DATAMATCH;
  uint64_t match = a->datamatch & len_mask(a->len);

  spinlock_acquire(&vm->lock);
  struct avm_ioeventfd *slot = NULL;
  err = 0;
  for (uint32_t i = 0; i < AVM_MAX_IOEVENTFDS; i++) {
    struct avm_ioeventfd *e = &vm->ioeventfds[i];
    if (!e->used) {
      if (!slot)
        slot = e;
    } else if (same_match(e, pio, a->addr, a->len, datamatch, match)) {
      err = -EEXIST;
      break;
    }
  }
  if (!err && !slot)
    err = -ENOSPC;
  if (!err) {
    slot->used = true;
    slot->pio = pio;
    slot->datamatch = datamatch;
    slot->len = a->len;
    slot->addr = a->addr;
    slot->match = match;
    slot->efd = node;
  }
  spinlock_release(&vm->lock);

  /* On success the slot keeps the reference taken above. */
  if (err)
    asc_eventfd_put(node);
  return err;
}

int avm_ioeventfd_deassign(struct avm_vm *vm, const struct kvm_ioeventfd *a) {
  void *node;
  int err = asc_eventfd_get(a->fd, &node);
  if (err)
    return err;

  bool pio = a->flags & KVM_IOEVENTFD_FLAG_PIO;
  bool datamatch = a->flags & KVM_IOEVENTFD_FLAG_DATAMATCH;
  uint64_t match = a->datamatch & len_mask(a->len);

  void *held = NULL;
  spinlock_acquire(&vm->lock);
  for (uint32_t i = 0; i < AVM_MAX_IOEVENTFDS; i++) {
    struct avm_ioeventfd *e = &vm->ioeventfds[i];
    if (e->used && e->efd == node &&
        same_match(e, pio, a->addr, a->len, datamatch, match)) {
      held = e->efd;
      e->used = false;
      break;
    }
  }
  spinlock_release(&vm->lock);

  asc_eventfd_put(node);
  if (!held)
    return -ENOENT;
  asc_eventfd_put(held);
  return 0;
}

bool avm_ioeventfd_write(struct avm_vm *vm, bool pio, uint64_t addr,
                         uint32_t len, uint64_t val) {
  bool hit = false;
  spinlock_acquire(&vm->lock);
  for (uint32_t i = 0; i < AVM_MAX_IOEVENTFDS; i++) {
    struct avm_ioeventfd *e = &vm->ioeventfds[i];
    if (!e->used || e->pio != pio || e->addr != addr)
      continue;
    if (e->len && e->len != len)
      continue;
    if (e->datamatch && (val & len_mask(len)) != e->match)
      continue;
    asc_eventfd_signal(e->efd, 0);
    hit = true;
    break;
  }
  spinlock_release(&vm->lock);
  return hit;
}

void avm_ioeventfd_destroy(struct avm_vm *vm) {
  for (uint32_t i = 0; i < AVM_MAX_IOEVENTFDS; i++) {
    struct avm_ioeventfd *e = &vm->ioeventfds[i];
    if (!e->used)
      continue;
    e->used = false;
    asc_eventfd_put(e->efd);
  }
}
