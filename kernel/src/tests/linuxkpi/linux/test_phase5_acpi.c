/* Phase 5 C3 — ACPI table accessor self-test.
 *
 * Exercises the LinuxKPI acpi_get_table()/acpi_put_table() shim over the
 * native RSDT/XSDT walker: FADT and MADT must resolve with sane signatures
 * and lengths, unknown signatures must fail cleanly, and put_table is a
 * safe no-op.  CONFIG_ACPI stays off, so this is groundwork for the
 * bare-metal VFCT/VBIOS path (the VFIO path uses the ROM BAR instead). */

#include <linux/acpi.h>
#include <linux/string.h>

#include <linuxkpi/log.h>

static int p5a_failures;

static void p5a_ok(const char *what) {
  klogf("[  OK  ] LinuxKPI: acpi %s\n", what);
}

static void p5a_fail(const char *what, long v) {
  p5a_failures++;
  klogf("[FAIL] LinuxKPI: acpi %s (%ld)\n", what, v);
}

void linuxkpi_test_phase5_acpi(void) {
  struct acpi_table_header *fadt = NULL, *madt = NULL, *missing = NULL;
  acpi_status status;

  p5a_failures = 0;
  klog_puts("[LINUXKPI] Phase 5 ACPI self-test\n");

  status = acpi_get_table(ACPI_SIG_FADT, 1, &fadt);
  if (ACPI_SUCCESS(status) && fadt && fadt->length >= sizeof(*fadt) &&
      !memcmp(fadt->signature, ACPI_SIG_FADT, 4))
    p5a_ok("FADT via native table walker");
  else
    p5a_fail("FADT", (long)status);
  if (fadt)
    klogf("[INFO] LinuxKPI: acpi FADT len=%u rev=%u\n", fadt->length,
          fadt->revision);

  status = acpi_get_table(ACPI_SIG_APIC, 0, &madt);
  if (ACPI_SUCCESS(status) && madt && madt->length >= sizeof(*madt) &&
      !memcmp(madt->signature, ACPI_SIG_APIC, 4))
    p5a_ok("MADT via native table walker");
  else
    p5a_fail("MADT", (long)status);

  status = acpi_get_table("ZZZZ", 0, &missing);
  if (!ACPI_SUCCESS(status) && missing == NULL)
    p5a_ok("unknown signature fails cleanly");
  else
    p5a_fail("unknown signature", (long)status);

  acpi_put_table(fadt);
  acpi_put_table(madt);
  p5a_ok("acpi_put_table is safe");

  if (p5a_failures == 0)
    klog_puts("[  OK  ] LinuxKPI: acpi suite complete\n");
  else
    klogf("[FAIL] LinuxKPI: acpi suite had %d failure(s)\n", p5a_failures);
}
