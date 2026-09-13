/* Native ACPI bridge implementation for LinuxKPI (Phase 5 C3).
 *
 * Compiled with native headers only; see linuxkpi/native_acpi.h. */

#include "linuxkpi/native_acpi.h"

#include "acpi/acpi.h"

#include <stddef.h>

void *linuxkpi_acpi_find_table(const char *signature) {
  char sig[5];

  if (!signature)
    return NULL;
  for (int i = 0; i < 4; i++)
    sig[i] = signature[i];
  sig[4] = '\0';
  return acpi_find_table(sig);
}

unsigned int linuxkpi_acpi_table_length(const void *header) {
  const struct acpi_sdt_header *h = header;

  return h ? h->length : 0;
}
