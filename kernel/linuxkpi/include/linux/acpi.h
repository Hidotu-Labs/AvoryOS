#ifndef __AVORY_LINUXKPI_ACPI_H
#define __AVORY_LINUXKPI_ACPI_H

/* AvoryOS overlay for <linux/acpi.h>.
 *
 * CONFIG_ACPI stays unset in the LinuxKPI config: the native ACPI layer only
 * parses MADT/FADT/MCFG/HPET, and imported drivers take their !CONFIG_ACPI
 * code paths.  Phase 5 adds the table-accessor subset on top of the native
 * `acpi_find_table()` walker so the VFCT/VBIOS groundwork is link-ready for
 * the bare-metal path (the VFIO path reads the ROM BAR instead).  There is no
 * AML interpreter; `acpi_evaluate_*` remains unavailable by design. */

#include <linux/types.h>

struct acpi_device;
struct acpi_bus_type;
struct acpi_buffer;
struct device;
struct fwnode_handle;

/* Included unconditionally by some upstream headers even with ACPI off. */
struct acpi_driver;

/* ACPICA status shape: 0 is AE_OK, anything else is a failure. */
typedef u32 acpi_status;
#define AE_OK 0
#define AE_NOT_FOUND 0x0005
#define ACPI_SUCCESS(a) (!(a))
#define ACPI_FAILURE(a) ((a) != AE_OK)

/* Layout matches the native `struct acpi_sdt_header` (and upstream actbl.h). */
struct acpi_table_header {
  char signature[4];
  u32 length;
  u8 revision;
  u8 checksum;
  char oem_id[6];
  char oem_table_id[8];
  u32 oem_revision;
  u32 creator_id;
  u32 creator_revision;
} __attribute__((packed));

#define ACPI_SIG_FADT "FACP"
#define ACPI_SIG_APIC "APIC"
#define ACPI_SIG_VFCT "VFCT"

/* Instance 0 and 1 both return the first table: the native walker keeps no
 * instance list, and real callers use 1 (amdgpu's VFCT lookup) while test
 * code commonly uses 0.  Recorded in docs/linuxkpi-gaps.md. */
acpi_status acpi_get_table(char *signature, u32 instance,
                           struct acpi_table_header **out_table);
void acpi_put_table(struct acpi_table_header *table);

#endif /* __AVORY_LINUXKPI_ACPI_H */
