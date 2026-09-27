#include "drivers/timer/pit.h"
#include "hal/hal.h"
#include "console/console.h"
#include "console/klog.h"
#include "cpu/irq.h"
#include "cpu/isr.h"
#include "io/io.h"
#include <stdbool.h>
#include <stdint.h>

#define PIT_CMD_PORT 0x43
#define PIT_DATA_PORT0 0x40

static uint64_t pit_ticks = 0;
static void pit_callback(struct registers *regs) {
  (void)regs;
  pit_ticks++;
}

void pit_init(uint32_t frequency) {
  // Register our callback for IRQ0 (Interrupt 32)
  irq_install_handler(0, pit_callback, 0);

  // Hardware clock is 1193180 Hz
  uint32_t divisor = 1193180 / frequency;

  // Send the command byte
  // 0x36: Channel 0, Access LSB then MSB, Mode 3 (Square Wave Generator),
  // 16-bit binary
  outb(PIT_CMD_PORT, 0x36);

  // Send the frequency divisor
  uint8_t l = (uint8_t)(divisor & 0xFF);
  uint8_t h = (uint8_t)((divisor >> 8) & 0xFF);

  outb(PIT_DATA_PORT0, l);
  outb(PIT_DATA_PORT0, h);
}

#include "../../apic/lapic_timer.h"

uint64_t pit_get_ticks(void) { return lapic_timer_get_ms() / 10; }

void pit_sleep(uint32_t ms) {
  // 100Hz = 10ms per tick
  uint64_t ticks_to_wait = ms / 10;
  if (ticks_to_wait == 0)
    ticks_to_wait = 1;

  uint64_t end = pit_ticks + ticks_to_wait;
  while (pit_ticks < end) {
    hal_cpu_halt();
  }
}
