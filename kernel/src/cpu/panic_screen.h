#ifndef CPU_PANIC_SCREEN_H
#define CPU_PANIC_SCREEN_H

#include <stdbool.h>
#include <stdint.h>

struct limine_framebuffer;
struct registers;

/* Cache the firmware scanout before graphics clients can take it over. */
void panic_screen_init(const struct limine_framebuffer *framebuffer);

/* Freeze peer CPUs before any lengthy diagnostics or framebuffer writes. */
bool panic_screen_stop_other_cpus(void);
bool panic_screen_is_active(void);
void panic_screen_ack_stopped_cpu(void);
uint32_t panic_screen_current_apic_id(void);
void panic_screen_stop_this_cpu(void) __attribute__((noreturn));

/* Draws directly to the cached scanout, bypassing console, DRM and fb locks. */
bool panic_screen_render(const char *reason, struct registers *regs,
                         bool has_cr2, uint64_t cr2);

#endif
