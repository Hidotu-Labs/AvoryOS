#ifndef DRIVERS_USB_USB_DEBUG_H
#define DRIVERS_USB_USB_DEBUG_H

#include <stdbool.h>

/* Verbose USB tracing.
 *
 * The per-transfer logs are invaluable while bringing up a device, but once
 * things work they drown the console and push the probe decisions out of
 * /proc/usb's boot-log excerpt.  They are off by default and can be turned on
 * at runtime:
 *
 *   echo 1 > /proc/usb_debug      # enable
 *   echo 0 > /proc/usb_debug      # disable
 *
 * or at boot with the kernel command line option `usbdebug`.
 *
 * The macros reference klog_puts()/klogf(), so include console/klog.h first.
 */
extern bool usb_verbose;

#define usb_dbg_puts(s)                                                        \
  do {                                                                         \
    if (usb_verbose)                                                           \
      klog_puts(s);                                                            \
  } while (0)
#define usb_dbg_uint64(n)                                                      \
  do {                                                                         \
    if (usb_verbose)                                                           \
      klog_uint64(n);                                                          \
  } while (0)
#define usb_dbg_hex32(n)                                                       \
  do {                                                                         \
    if (usb_verbose)                                                           \
      klog_hex32(n);                                                           \
  } while (0)
#define usb_dbg_int64(n)                                                       \
  do {                                                                         \
    if (usb_verbose)                                                           \
      klog_int64(n);                                                           \
  } while (0)
#define usb_dbgf(...)                                                          \
  do {                                                                         \
    if (usb_verbose)                                                           \
      klogf(__VA_ARGS__);                                                      \
  } while (0)

#endif
