#ifndef __AVORY_LINUXKPI_KSTRTOX_H
#define __AVORY_LINUXKPI_KSTRTOX_H

/* Additive Linux <linux/kstrtox.h> overlay: lib/kstrtox.c uses min()/max()
 * that the real kernel.h pulls in transitively, which this layer does not. */

#include <linux/minmax.h>
#include <linux/types.h>

#include_next <linux/kstrtox.h>

#endif /* __AVORY_LINUXKPI_KSTRTOX_H */
