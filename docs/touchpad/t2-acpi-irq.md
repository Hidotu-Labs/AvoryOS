# T2 — Discover the controller and client through ACPI

**Status:** Planned.

**Dependency:** T0 hardware baseline.

## Implementation

Resolve `SYNA2B4E` to the firmware controller represented by `AMD0010:00` on Linux. Record the address, descriptor register, and IRQ/GPIO route from the target firmware.


- Evaluate an embeddable AML interpreter against the first target's DSDT and SSDTs. Compare its kernel integration requirements, licensing, and ability to evaluate the target's firmware methods. Use the [native ACPI layer](../../kernel/src/acpi/acpi.c) for table access and expose a narrow resource interface to LinuxKPI.
- Resolve device identity and presence, controller resources, and the client connection from `_HID`/`_CID`, `_STA`, and `_CRS`. Resolve the I²C controller reference, bus address and speed, and the HID descriptor register through the applicable firmware method.
- Map direct and GPIO interrupts through the appropriate providers, honoring polarity and trigger mode. Implement the firmware power or dependency methods that the selected hardware requires. Defer binding until the resources the device needs are available.

**Exit:** AvoryOS logs a firmware-derived controller, client address, descriptor register, and interrupt route that agree with the Linux baseline. Missing providers produce a clear diagnostic. [Linux ACPI enumeration](https://kernel.org/doc/html/v6.6/firmware-guide/acpi/enumeration.html)

## Evidence to record

Record firmware table identifiers, the resolved resource path, and the comparison with Linux. Include evidence for deferred binding and IRQ trigger/polarity.
