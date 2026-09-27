#ifndef IO_H
#define IO_H

#include "hal/hal.h"

static inline void outb(uint16_t port, uint8_t val) {
    hal_port_write8(port, val);
}

static inline uint8_t inb(uint16_t port) {
    return hal_port_read8(port);
}

static inline void outw(uint16_t port, uint16_t val) {
    hal_port_write16(port, val);
}

static inline uint16_t inw(uint16_t port) {
    return hal_port_read16(port);
}

static inline void outl(uint16_t port, uint32_t val) {
    hal_port_write32(port, val);
}

static inline uint32_t inl(uint16_t port) {
    return hal_port_read32(port);
}

static inline void insw(uint16_t port, void *buf, uint32_t count) {
    hal_port_read16s(port, buf, count);
}

static inline void outsw(uint16_t port, const void *buf, uint32_t count) {
    hal_port_write16s(port, buf, count);
}

static inline void io_wait(void) {
    hal_io_wait();
}

#endif
