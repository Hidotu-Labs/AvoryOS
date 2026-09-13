#ifndef __AVORY_LINUXKPI_FIRMWARE_H
#define __AVORY_LINUXKPI_FIRMWARE_H

/* AvoryOS overlay for <linux/firmware.h>.
 *
 * The stock header's !CONFIG_FW_UPLOAD inline stubs use ERR_PTR()/EINVAL
 * without including <linux/err.h>/<linux/errno.h>, relying on the includer.
 * amdgpu_ucode.c includes firmware.h first, so pull them explicitly and then
 * the stock header.  No other change. */

#include <linux/err.h>
#include <linux/errno.h>

#include_next <linux/firmware.h>

#endif /* __AVORY_LINUXKPI_FIRMWARE_H */
