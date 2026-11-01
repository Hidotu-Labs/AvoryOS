#ifndef AVM_AVM_IOEVENTFD_H
#define AVM_AVM_IOEVENTFD_H

#include "avm_internal.h"

#include <stdbool.h>
#include <stdint.h>

/* Provided by syscalls/sys_aio.c.  asc_eventfd_get takes a reference on the
 * eventfd behind fd and asc_eventfd_put drops it.  asc_eventfd_signal adds one
 * to its counter and wakes its readers. */
int asc_eventfd_get(int fd, void **result);
void asc_eventfd_put(void *node);
void asc_eventfd_signal(void *opaque, unsigned int mask);
int asc_eventfd_set_notify(void *opaque, void (*fn)(void *arg), void *arg);
void asc_eventfd_clear_notify(void *opaque);

/* KVM_IOEVENTFD.  Assign and deassign take a validated request. */
int avm_ioeventfd_assign(struct avm_vm *vm, const struct kvm_ioeventfd *a);
int avm_ioeventfd_deassign(struct avm_vm *vm, const struct kvm_ioeventfd *a);

/* Signals the matching eventfd and returns true when a guest write is one of
 * the registered ones.  Only writes match; the caller then resumes the guest. */
bool avm_ioeventfd_write(struct avm_vm *vm, bool pio, uint64_t addr,
                         uint32_t len, uint64_t val);

/* Drops every registration and its eventfd reference.  Called when the VM is
 * freed. */
void avm_ioeventfd_destroy(struct avm_vm *vm);

#endif /* AVM_AVM_IOEVENTFD_H */
