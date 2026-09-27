#ifndef ASCENTOS_HAL_H
#define ASCENTOS_HAL_H

#include <stdbool.h>
#include <stdint.h>

/* Saved architecture interrupt state returned by hal_irq_save(). */
typedef uintptr_t hal_irq_state_t;

/* HAL lifecycle and architecture information. */
bool hal_init(void);
bool hal_is_initialized(void);
const char *hal_arch_name(void);

/* CPU execution primitives. */
void hal_cpu_relax(void);
void hal_cpu_halt(void);
uint64_t hal_cpu_cycle_count(void);

/* Local CPU interrupt control. */
void hal_irq_enable(void);
void hal_irq_disable(void);
bool hal_irq_enabled(void);
hal_irq_state_t hal_irq_save(void);
void hal_irq_restore(hal_irq_state_t state);

/* Legacy port-mapped I/O. */
void hal_port_write8(uint16_t port, uint8_t value);
void hal_port_write16(uint16_t port, uint16_t value);
void hal_port_write32(uint16_t port, uint32_t value);
uint8_t hal_port_read8(uint16_t port);
uint16_t hal_port_read16(uint16_t port);
uint32_t hal_port_read32(uint16_t port);
void hal_port_read16s(uint16_t port, void *buffer, uint32_t count);
void hal_port_write16s(uint16_t port, const void *buffer, uint32_t count);
void hal_io_wait(void);

#endif
