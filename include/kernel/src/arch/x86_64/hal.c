#include "hal/hal.h"
#include "hal/hal_arch.h"

#define X86_RFLAGS_IF (1UL << 9)

bool hal_arch_init(void) { return true; }

const char *hal_arch_name_impl(void) { return "x86_64"; }

void hal_cpu_relax(void) { __asm__ volatile("pause" ::: "memory"); }

void hal_cpu_halt(void) { __asm__ volatile("hlt"); }

uint64_t hal_cpu_cycle_count(void) {
  uint32_t low;
  uint32_t high;

  __asm__ volatile("rdtsc" : "=a"(low), "=d"(high));
  return ((uint64_t)high << 32) | low;
}

void hal_irq_enable(void) { __asm__ volatile("sti" ::: "memory"); }

void hal_irq_disable(void) { __asm__ volatile("cli" ::: "memory"); }

bool hal_irq_enabled(void) {
  uintptr_t flags;

  __asm__ volatile("pushfq; pop %0" : "=r"(flags));
  return (flags & X86_RFLAGS_IF) != 0;
}

hal_irq_state_t hal_irq_save(void) {
  uintptr_t flags;

  __asm__ volatile("pushfq; pop %0; cli" : "=r"(flags) : : "memory");
  return flags;
}

void hal_irq_restore(hal_irq_state_t state) {
  if (state & X86_RFLAGS_IF)
    hal_irq_enable();
}

void hal_port_write8(uint16_t port, uint8_t value) {
  __asm__ volatile("outb %0, %1" : : "a"(value), "Nd"(port));
}

void hal_port_write16(uint16_t port, uint16_t value) {
  __asm__ volatile("outw %0, %1" : : "a"(value), "Nd"(port));
}

void hal_port_write32(uint16_t port, uint32_t value) {
  __asm__ volatile("outl %0, %1" : : "a"(value), "Nd"(port));
}

uint8_t hal_port_read8(uint16_t port) {
  uint8_t value;

  __asm__ volatile("inb %1, %0" : "=a"(value) : "Nd"(port));
  return value;
}

uint16_t hal_port_read16(uint16_t port) {
  uint16_t value;

  __asm__ volatile("inw %1, %0" : "=a"(value) : "Nd"(port));
  return value;
}

uint32_t hal_port_read32(uint16_t port) {
  uint32_t value;

  __asm__ volatile("inl %1, %0" : "=a"(value) : "Nd"(port));
  return value;
}

void hal_port_read16s(uint16_t port, void *buffer, uint32_t count) {
  __asm__ volatile("rep insw"
                   : "+D"(buffer), "+c"(count)
                   : "d"(port)
                   : "memory");
}

void hal_port_write16s(uint16_t port, const void *buffer, uint32_t count) {
  __asm__ volatile("rep outsw"
                   : "+S"(buffer), "+c"(count)
                   : "d"(port)
                   : "memory");
}

void hal_io_wait(void) { hal_port_write8(0x80, 0); }
