#ifndef LINUXKPI_NATIVE_ACPI_H
#define LINUXKPI_NATIVE_ACPI_H

/* Native ACPI bridge for LinuxKPI implementation files.
 *
 * The native layer (kernel/src/acpi) only walks RSDT/XSDT and finds a table
 * by 4-character signature; these entry points hide the native structures so
 * linuxkpi/src/acpi.c can stay a Linux-API translation unit.  Only builtin
 * types are used. */

/* Returns the native table header pointer for `signature` (4 chars) or NULL. */
void *linuxkpi_acpi_find_table(const char *signature);

/* `length` field of a returned table header (0 when NULL). */
unsigned int linuxkpi_acpi_table_length(const void *header);

#endif /* LINUXKPI_NATIVE_ACPI_H */
