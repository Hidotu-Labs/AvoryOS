#ifndef __AVORY_LINUXKPI_WAIT_BIT_H
#define __AVORY_LINUXKPI_WAIT_BIT_H

/* Minimal Linux <linux/wait_bit.h> overlay.
 *
 * Bit waits are backed by a small address-hashed wait-queue table in
 * linuxkpi/src/wait_bit.c; wake_up_bit()/wake_up_var() wake the bucket for
 * the address.  Same API shape as Linux, coarser wakeups. */

#include <linux/types.h>

int wait_on_bit(unsigned long *word, int bit, unsigned int mode);
int wait_on_bit_io(unsigned long *word, int bit, unsigned int mode);
int wait_on_bit_timeout(unsigned long *word, int bit, unsigned int mode,
                        unsigned long timeout);
int wait_on_bit_action(unsigned long *word, int bit,
                       int (*action)(void *), unsigned int mode);

int wait_on_bit_lock(unsigned long *word, int bit);
int wait_on_bit_lock_io(unsigned long *word, int bit);
int wait_on_bit_lock_action(unsigned long *word, int bit,
                            int (*action)(void *));

void wake_up_bit(void *word, int bit);
void wake_up_var(void *var);

#endif /* __AVORY_LINUXKPI_WAIT_BIT_H */
