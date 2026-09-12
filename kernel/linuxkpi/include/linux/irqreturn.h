#ifndef __AVORY_LINUXKPI_IRQRETURN_H
#define __AVORY_LINUXKPI_IRQRETURN_H

/* Upstream-compatible <linux/irqreturn.h>.  The enum must match upstream
 * exactly: devm_request_irq()'s prototype (interrupt.h) and headers such as
 * drm_drv.h both spell the handler type, and a typedef that is not the enum
 * makes them conflicting types. */

enum irqreturn {
  IRQ_NONE = 0,
  IRQ_HANDLED = 1,
  IRQ_WAKE_THREAD = 2,
};

typedef enum irqreturn irqreturn_t;

#endif /* __AVORY_LINUXKPI_IRQRETURN_H */
