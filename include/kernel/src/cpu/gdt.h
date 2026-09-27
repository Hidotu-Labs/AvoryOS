#ifndef CPU_GDT_H
#define CPU_GDT_H

#include <stdint.h>

struct gdt_entry {
  uint16_t limit_low;
  uint16_t base_low;
  uint8_t base_middle;
  uint8_t access;
  uint8_t granularity;
  uint8_t base_high;
} __attribute__((packed));

struct gdt_ptr {
  uint16_t limit;
  uint64_t base;
} __attribute__((packed));

struct tss_entry {
  uint32_t reserved0;
  uint64_t rsp0;
  uint64_t rsp1;
  uint64_t rsp2;
  uint64_t reserved1;
  uint64_t ist1;
  uint64_t ist2;
  uint64_t ist3;
  uint64_t ist4;
  uint64_t ist5;
  uint64_t ist6;
  uint64_t ist7;
  uint64_t reserved2;
  uint16_t reserved3;
  uint16_t iopb_offset;
} __attribute__((packed));

void tss_set_rsp0(uint64_t rsp0);

void gdt_init(void);
void gdt_load_ap(uint32_t cpu_id);
void cpu_switch_stack(uint64_t new_rsp);
void cpu_jump_to_stack(uint64_t new_rsp, void (*target)(void));

#endif
