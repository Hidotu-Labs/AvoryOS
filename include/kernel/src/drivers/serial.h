#ifndef DRIVERS_SERIAL_H
#define DRIVERS_SERIAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

void serial_init(void);
void serial_putchar(char c);
void serial_write(const char *data, size_t length);
void serial_flush(void);
uint32_t serial_pending_bytes(void);
void serial_flush_sync(void);
void serial_putchar_sync(char c);
void serial_write_sync(const char *data, size_t length);
int serial_received(void);
int serial_try_get_char(void);
char serial_get_char(void);

#endif
