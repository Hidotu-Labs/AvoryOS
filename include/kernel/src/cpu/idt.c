#include "idt.h"
#include <stddef.h>

static struct idt_entry idt[256];
static struct idt_ptr idtp;

extern void isr0(void);
extern void isr1(void);
extern void isr2(void);
extern void isr3(void);
extern void isr4(void);
extern void isr5(void);
extern void isr6(void);
extern void isr7(void);
extern void isr8(void);
extern void isr9(void);
extern void isr10(void);
extern void isr11(void);
extern void isr12(void);
extern void isr13(void);
extern void isr14(void);
extern void isr15(void);
extern void isr16(void);
extern void isr17(void);
extern void isr18(void);
extern void isr19(void);
extern void isr20(void);
extern void isr21(void);
extern void isr22(void);
extern void isr23(void);
extern void isr24(void);
extern void isr25(void);
extern void isr26(void);
extern void isr27(void);
extern void isr28(void);
extern void isr29(void);
extern void isr30(void);
extern void isr31(void);
extern void isr32(void);
extern void isr33(void);
extern void isr34(void);
extern void isr35(void);
extern void isr36(void);
extern void isr37(void);
extern void isr38(void);
extern void isr39(void);
extern void isr40(void);
extern void isr41(void);
extern void isr42(void);
extern void isr43(void);
extern void isr44(void);
extern void isr45(void);
extern void isr46(void);
extern void isr47(void);
extern void isr48(void);
extern void isr49(void);
extern void isr50(void);
extern void isr255(void);

void idt_set_gate(uint8_t num, uint64_t base, uint16_t sel, uint8_t flags) {
  idt[num].base_low = (base & 0xFFFF);
  idt[num].base_mid = (base >> 16) & 0xFFFF;
  idt[num].base_high = (base >> 32) & 0xFFFFFFFF;
  idt[num].sel = sel;
  idt[num].always0 = 0;
  idt[num].flags = flags;
  idt[num].ist = 0;
}

void idt_init(void) {
  idtp.limit = (sizeof(struct idt_entry) * 256) - 1;
  idtp.base = (uint64_t)&idt;

  for (int i = 0; i < 256; i++) {
    idt_set_gate(i, 0, 0, 0);
  }

  uint8_t flags = 0x8E; // Present, Ring 0, Interrupt Gate
  uint16_t sel = 0x08;  // Kernel Code Segment

  /*
   * #BP and #OF are the only exceptions raised directly by user-mode
   * instructions (int3 and into).  Their gates must have DPL 3: with a
   * DPL 0 gate the CPU turns a user "int3" into a #GP whose error code is
   * the gate selector ((3 << 3) | 2 = 0x1A), which then looks nothing like
   * a breakpoint.  GLib's G_BREAKPOINT() is a plain int3, so this is what
   * a g_error()/g_assert() in a user program used to crash with.  Linux
   * gives these two gates DPL 3 for the same reason.
   */
  uint8_t user_flags = flags | 0x60; // Present, Ring 3, Interrupt Gate

  idt_set_gate(0, (uint64_t)isr0, sel, flags);
  idt_set_gate(1, (uint64_t)isr1, sel, flags);
  idt_set_gate(2, (uint64_t)isr2, sel, flags);
  idt_set_gate(3, (uint64_t)isr3, sel, user_flags);
  idt_set_gate(4, (uint64_t)isr4, sel, user_flags);
  idt_set_gate(5, (uint64_t)isr5, sel, flags);
  idt_set_gate(6, (uint64_t)isr6, sel, flags);
  idt_set_gate(7, (uint64_t)isr7, sel, flags);
  idt_set_gate(8, (uint64_t)isr8, sel, flags);
  idt_set_gate(9, (uint64_t)isr9, sel, flags);
  idt_set_gate(10, (uint64_t)isr10, sel, flags);
  idt_set_gate(11, (uint64_t)isr11, sel, flags);
  idt_set_gate(12, (uint64_t)isr12, sel, flags);
  idt_set_gate(13, (uint64_t)isr13, sel, flags);
  idt_set_gate(14, (uint64_t)isr14, sel, flags);
  idt_set_gate(15, (uint64_t)isr15, sel, flags);
  idt_set_gate(16, (uint64_t)isr16, sel, flags);
  idt_set_gate(17, (uint64_t)isr17, sel, flags);
  idt_set_gate(18, (uint64_t)isr18, sel, flags);
  idt_set_gate(19, (uint64_t)isr19, sel, flags);
  idt_set_gate(20, (uint64_t)isr20, sel, flags);
  idt_set_gate(21, (uint64_t)isr21, sel, flags);
  idt_set_gate(22, (uint64_t)isr22, sel, flags);
  idt_set_gate(23, (uint64_t)isr23, sel, flags);
  idt_set_gate(24, (uint64_t)isr24, sel, flags);
  idt_set_gate(25, (uint64_t)isr25, sel, flags);
  idt_set_gate(26, (uint64_t)isr26, sel, flags);
  idt_set_gate(27, (uint64_t)isr27, sel, flags);
  idt_set_gate(28, (uint64_t)isr28, sel, flags);
  idt_set_gate(29, (uint64_t)isr29, sel, flags);
  idt_set_gate(30, (uint64_t)isr30, sel, flags);
  idt_set_gate(31, (uint64_t)isr31, sel, flags);
  idt_set_gate(32, (uint64_t)isr32, sel, flags);
  idt_set_gate(33, (uint64_t)isr33, sel, flags);
  idt_set_gate(34, (uint64_t)isr34, sel, flags);
  idt_set_gate(35, (uint64_t)isr35, sel, flags);
  idt_set_gate(36, (uint64_t)isr36, sel, flags);
  idt_set_gate(37, (uint64_t)isr37, sel, flags);
  idt_set_gate(38, (uint64_t)isr38, sel, flags);
  idt_set_gate(39, (uint64_t)isr39, sel, flags);
  idt_set_gate(40, (uint64_t)isr40, sel, flags);
  idt_set_gate(41, (uint64_t)isr41, sel, flags);
  idt_set_gate(42, (uint64_t)isr42, sel, flags);
  idt_set_gate(43, (uint64_t)isr43, sel, flags);
  idt_set_gate(44, (uint64_t)isr44, sel, flags);
  idt_set_gate(45, (uint64_t)isr45, sel, flags);
  idt_set_gate(46, (uint64_t)isr46, sel, flags);
  idt_set_gate(47, (uint64_t)isr47, sel, flags);

  // LAPIC timer interrupt vector
  idt_set_gate(48, (uint64_t)isr48, sel, flags);

  // Reschedule IPI vector
  idt_set_gate(49, (uint64_t)isr49, sel, flags);

  // TLB shootdown IPI vector
  idt_set_gate(50, (uint64_t)isr50, sel, flags);

  extern void *isr_dynamic_stub_table[];
  for (uint16_t vector = 51; vector < 255; vector++)
    idt_set_gate((uint8_t)vector,
                 (uint64_t)isr_dynamic_stub_table[vector - 51], sel, flags);

  // LAPIC spurious interrupt vector
  idt_set_gate(255, (uint64_t)isr255, sel, flags);

  __asm__ volatile("lidt %0" : : "m"(idtp));
}

void idt_load(void) {
  // Just load the already-populated IDT on this CPU.
  // Used by APs — they must NOT call idt_init() because that zeros and
  // rebuilds the shared IDT table, racing with the BSP's timer interrupts.
  __asm__ volatile("lidt %0" : : "m"(idtp));
}
