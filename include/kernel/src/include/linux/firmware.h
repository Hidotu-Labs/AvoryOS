/* SPDX-License-Identifier: GPL-2.0 */
#ifndef ASCENT_LINUX_FIRMWARE_H
#define ASCENT_LINUX_FIRMWARE_H

#include <stddef.h>
#include <stdint.h>

struct device;

/* Linux-compatible ownership shape: release_firmware() frees this object. */
struct firmware {
  size_t size;
  const uint8_t *data;
};

int request_firmware(const struct firmware **firmware_p, const char *name,
                     const struct device *device);
void release_firmware(const struct firmware *firmware);

#endif
