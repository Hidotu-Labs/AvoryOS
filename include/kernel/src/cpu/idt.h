#ifndef CPU_IDT_H
#define CPU_IDT_H

#include <stdint.h>

struct idt_entry {
  uint16_t base_low;
  uint16_t sel;
  uint8_t ist;
  uint8_t flags;
  uint16_t base_mid;
  uint32_t base_high;
  uint32_t always0;
} __attribute__((packed));

struct idt_ptr {
  uint16_t limit;
  uint64_t base;
} __attribute__((packed));

void idt_set_gate(uint8_t num, uint64_t base, uint16_t sel, uint8_t flags);
void idt_init(void);
void idt_load(void);

#endif
