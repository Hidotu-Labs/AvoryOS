#ifndef __AVORY_LINUXKPI_THREAD_INFO_H
#define __AVORY_LINUXKPI_THREAD_INFO_H

/* Minimal Linux <linux/thread_info.h> overlay: no thread_info structure or
 * current_thread_info() yet (imported code uses `current` instead). */

#include <linux/types.h>

/* Referenced by asm/thread_info.h's arch_within_stack_frames(); values match
 * upstream. */
enum {
  BAD_STACK = -1,
  NOT_STACK = 0,
  GOOD_FRAME,
  GOOD_STACK,
};

#endif /* __AVORY_LINUXKPI_THREAD_INFO_H */
