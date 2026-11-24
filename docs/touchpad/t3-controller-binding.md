# T3 — Bring up the controller and LinuxKPI client binding

**Status:** Planned.

**Dependency:** T0 driver selection and T2 firmware resources for physical probe.

## Implementation

Qualify the driver that Linux binds to `AMD0010:00` on Stoney Ridge first. Confirm the driver and its dependencies in T0 before selecting import objects. Read the HID descriptor from the firmware-discovered `SYNA2B4E` client.


- Import the controller driver selected in Phase 0 and supply its PCI or platform glue. Implement the clock, reset, pin configuration, and power dependencies required by that controller.
- Extend the [LinuxKPI I²C core](../../kernel/linuxkpi/src/i2c.c) with firmware-backed client creation, driver matching, probe failure cleanup, and ordered removal. Keep client identity tied to the firmware controller reference so enumeration order cannot select the wrong bus.
- Support combined write/read transfers, bounded timeouts, and the adapter limits required by the driver. Verify controller ownership and IRQ cleanup on errors, and preserve existing graphics adapter behavior.

- Add independent default-off build and runtime binding gates for the bring-up. Restrict experimental binding to the selected controller and firmware client, and release native ownership after any failed probe.

**Exit:** Read a valid HID descriptor from the enumerated client on the first laptop. A failed transfer returns a bounded error, removing the adapter releases its clients and IRQs, and graphics DDC/EDID still works.

## Evidence to record

Record the import manifest and generated objects, build and boot gates, adapter and client identity, descriptor read, timeout and cleanup results, and graphics regression results.
