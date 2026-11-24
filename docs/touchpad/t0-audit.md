# T0 — Establish the hardware baseline

**Status:** Planned.

**Dependency:** None.

## First target

| Field | Observed on Linux |
|---|---|
| Platform | AMD Stoney Ridge |
| Touchpad | Synaptics TM3336-001 |
| Transport | HID over I²C |
| Touchpad ACPI ID | `SYNA2B4E` |
| Controller instance | `AMD0010:00` |
| HID vendor/product | `06CB:8251` |
| Input node observed on Linux | `/dev/input/event8` |

Collect the exact laptop and BIOS versions, bound Linux controller and HID drivers, I²C address, descriptor register, and IRQ/GPIO route for this machine. Resolve the observed input node through sysfs when collecting the baseline, since its event number can change.

## Implementation


- Use the Stoney Ridge laptop as the first AMD target. Pick an Intel laptop and a third machine with a different touchpad implementation or interrupt route for qualification.
- On Linux, capture the laptop and BIOS versions, controller PCI or ACPI ID, I²C client ID and address, bound drivers, relevant ACPI tables, interrupt route, and libinput device capabilities. Confirm motion, clicking, two-finger scroll, and resume where supported.
- Record the supported and untested combinations by controller, GPIO provider, touchpad ID, and firmware version.

- Run a focused import and compile/link spike for the first controller and HID stack. Record required Kbuild objects and unresolved API behavior before committing to the port.

**Exit:** Record the Linux baseline for each target, confirm the first machine’s firmware and driver path, and save the version lock, compile/link results, and dependency ledger.

## Evidence to record

Record the AvoryOS commit, pinned Linux tag and commit, hardware matrix, import and compile/link results, and an API dependency ledger with an owning phase for each gap.
