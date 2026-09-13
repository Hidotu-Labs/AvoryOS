/* Linux ACPI table access for LinuxKPI (Phase 5 C3).
 *
 * A thin adapter over the native RSDT/XSDT walker.  There is no AML
 * interpreter and no instance list: `acpi_get_table()` returns the first table
 * with the requested 4-character signature, and `acpi_put_table()` is a no-op
 * because the native tables live for the kernel's lifetime.  This is enough
 * for the VFCT/VBIOS groundwork and for the FADT/MADT self-test; amdgpu's
 * ACPI file itself stays compiled out while CONFIG_ACPI=n (see the gap log). */

#include <linux/acpi.h>
#include <linux/string.h>

#include <linuxkpi/native_acpi.h>

acpi_status acpi_get_table(char *signature, u32 instance,
                           struct acpi_table_header **out_table) {
  void *header;

  if (out_table)
    *out_table = NULL;
  if (!signature || !out_table)
    return AE_NOT_FOUND;
  if (instance > 1)
    return AE_NOT_FOUND;

  header = linuxkpi_acpi_find_table(signature);
  if (!header)
    return AE_NOT_FOUND;

  /* The native walker already checksum-validated the table. */
  *out_table = header;
  return AE_OK;
}

void acpi_put_table(struct acpi_table_header *table) { (void)table; }
