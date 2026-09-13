#ifndef __AVORY_LINUXKPI_IRQDOMAIN_H
#define __AVORY_LINUXKPI_IRQDOMAIN_H

/* AvoryOS overlay for <linux/irqdomain.h>.
 *
 * amdgpu_irq.h includes this header immediately before amdgpu_ih.h, which
 * embeds wait_queue_head_t and struct work_struct fields and relies on them
 * being in scope (upstream reaches them through other include chains).  Pull
 * them in first, then the stock header, whose declarations are used. */

#include <linux/wait.h>
#include <linux/workqueue.h>

#include_next <linux/irqdomain.h>

#endif /* __AVORY_LINUXKPI_IRQDOMAIN_H */
