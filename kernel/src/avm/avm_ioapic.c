#include "avm_ioapic.h"

#include "avm_lapic.h"

#include "../lib/string.h"

#define IOAPIC_PINS 24
#define IOREGSEL 0x00
#define IOWIN 0x10

#define REG_ID 0x00
#define REG_VER 0x01
#define REG_ARB 0x02
#define REG_REDIR_FIRST 0x10

/* Redirection entry layout.  Low word: vector 7:0, delivery mode 10:8,
 * destination mode 11, polarity 13, trigger 15, mask 16.  Bit 12 (delivery
 * status) and bit 14 (remote IRR) are read-only.  High word: destination in
 * bits 31:24. */
#define REDIR_MASK (1ull << 16)
#define REDIR_LOW_WRITABLE 0x1AFFFull
#define REDIR_HIGH_WRITABLE 0xFF000000ull

/* Version 0x11 with 24 redirection entries (max index 23). */
#define IOAPIC_VERSION ((uint32_t)(IOAPIC_PINS - 1) << 16 | 0x11)

void avm_ioapic_init(struct avm_vm *vm) {
  memset(&vm->ioapic, 0, sizeof(vm->ioapic));
  for (int i = 0; i < IOAPIC_PINS; i++)
    vm->ioapic.redir[i] = REDIR_MASK;
}

int avm_ioapic_mmio_read(struct avm_vm *vm, uint32_t off, uint32_t *out) {
  struct avm_ioapic *io = &vm->ioapic;
  if (off == IOREGSEL) {
    *out = io->index;
    return 0;
  }
  if (off != IOWIN) {
    *out = 0;
    return 0;
  }

  uint32_t idx = io->index & 0xFF;
  if (idx == REG_ID || idx == REG_ARB)
    *out = 0;  /* the ID is fixed at 0 */
  else if (idx == REG_VER)
    *out = IOAPIC_VERSION;
  else if (idx >= REG_REDIR_FIRST && idx < REG_REDIR_FIRST + 2 * IOAPIC_PINS) {
    uint64_t entry = io->redir[(idx - REG_REDIR_FIRST) >> 1];
    *out = (idx & 1) ? (uint32_t)(entry >> 32) : (uint32_t)entry;
  } else {
    *out = 0;
  }
  return 0;
}

int avm_ioapic_mmio_write(struct avm_vm *vm, uint32_t off, uint32_t val) {
  struct avm_ioapic *io = &vm->ioapic;
  if (off == IOREGSEL) {
    io->index = val & 0xFF;
    return 0;
  }
  if (off != IOWIN)
    return 0;

  uint32_t idx = io->index & 0xFF;
  if (idx < REG_REDIR_FIRST || idx >= REG_REDIR_FIRST + 2 * IOAPIC_PINS)
    return 0;  /* ID, version and arbitration are read-only here */

  uint64_t *entry = &io->redir[(idx - REG_REDIR_FIRST) >> 1];
  if (idx & 1)
    *entry = (*entry & 0xFFFFFFFFull) | (((uint64_t)val & REDIR_HIGH_WRITABLE) << 32);
  else
    *entry = (*entry & 0xFFFFFFFF00000000ull) | ((uint64_t)val & REDIR_LOW_WRITABLE);
  return 0;
}

/* Delivers a pin's entry as a fixed interrupt to the LAPIC it names.  Only the
 * fixed delivery mode and physical destinations are modelled. */
static void deliver(struct avm_vm *vm, uint32_t pin) {
  uint64_t entry = vm->ioapic.redir[pin];
  if (entry & REDIR_MASK)
    return;
  if ((entry >> 8) & 7)
    return;  /* not a fixed interrupt */

  uint8_t vector = (uint8_t)entry;
  uint8_t dest = (uint8_t)(entry >> 56);
  for (uint32_t i = 0; i < AVM_MAX_VCPUS; i++) {
    struct avm_vcpu *vcpu = vm->vcpus[i];
    if (vcpu && (dest == 0xFF || dest == (uint8_t)vcpu->id))
      avm_lapic_raise(vcpu, vector);
  }
}

void avm_ioapic_set_pin(struct avm_vm *vm, uint32_t pin, bool level) {
  if (pin >= IOAPIC_PINS)
    return;
  uint32_t bit = 1u << pin;
  if (level) {
    if (!(vm->ioapic.line & bit))
      deliver(vm, pin);
    vm->ioapic.line |= bit;
  } else {
    vm->ioapic.line &= ~bit;
  }
}
