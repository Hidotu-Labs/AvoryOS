#include "console/debug.h"

#if DEBUG_CONSOLE

#include "console/console.h"
#include "drivers/serial.h"
#include "lib/string.h"
#include <stdarg.h>

/*
 * The console path renders the message on the framebuffer and mirrors the
 * rendered bytes to COM1 itself, so a successful write must NOT also call
 * serial_write_sync(): that would print every debug line twice in a serial
 * capture.  The synchronous serial write is only the fallback for when the
 * console lock is held (wedged CPU, fault path), where screen output is not
 * safe but the UART still is.
 */
static void dbg_emit(const char *s, size_t len) {
  if (console_write_batch_try(s, len)) {
    /* Debug checkpoints must be on screen before the next statement runs:
     * whatever is visible when the machine wedges is the last line we got to
     * execute, not a frame that is still sitting in the backbuffer. */
    console_tick();
    return;
  }
  serial_write_sync(s, len);
}

void dbg_puts(const char *s) {
  if (!s)
    return;
  dbg_emit(s, strlen(s));
}

void dbg_printf(const char *fmt, ...) {
  char buf[256];
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  if (n < 0)
    return;
  if ((size_t)n >= sizeof(buf))
    n = (int)sizeof(buf) - 1;
  dbg_emit(buf, (size_t)n);
}

#endif /* DEBUG_CONSOLE */
