#ifndef __AVORY_LINUXKPI_ACPI_H
#define __AVORY_LINUXKPI_ACPI_H

/* AvoryOS overlay for <linux/acpi.h>.
 *
 * CONFIG_ACPI is intentionally unset in the LinuxKPI config: the native ACPI
 * layer only parses MADT/FADT/MCFG/HPET, and imported drivers take their
 * !CONFIG_ACPI code paths.  This header only has to satisfy the unconditional
 * include in files such as drm_sysfs.c; the ACPI-backed branches are compiled
 * out.  Phase 5 grows this into the table/VBIOS API amdgpu needs. */

#include <linux/types.h>

struct acpi_device;
struct acpi_bus_type;
struct acpi_buffer;
struct acpi_table_header;
struct device;
struct fwnode_handle;

/* Included unconditionally by some upstream headers even with ACPI off. */
struct acpi_driver;

#endif /* __AVORY_LINUXKPI_ACPI_H */
