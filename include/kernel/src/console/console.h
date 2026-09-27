#ifndef CONSOLE_CONSOLE_H
#define CONSOLE_CONSOLE_H

#include <limine.h>
#include <stdbool.h>
#include <stddef.h>

void console_init(struct limine_framebuffer *framebuffer);
void console_putchar(char c);
void console_puts(const char *s);

// Batch write: renders all `len` bytes then does a single backbuffer swap.
// Use this from sys_write / VFS write to eliminate per-character flicker.
void console_write_batch(const char *buf, size_t len);

void console_clear(void);
void console_set_cursor_visible(bool visible);
void console_refresh_cursor(void);
void console_redraw_all(void);
void console_scroll_view(int delta);
uint32_t console_get_rows(void);

#endif
