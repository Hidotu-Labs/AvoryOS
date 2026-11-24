# Laptop touchpad implementation plan

## Goal and release scope

AvoryOS should discover internal touchpads at boot and expose them to libinput as touchpads with real multitouch contacts. The first release targets HID over I²C on x86 laptops. Add support across multiple controller and GPIO implementations, and publish the tested hardware matrix. Vendor-specific I²C and SMBus protocols, SPI, and improvements to legacy PS/2 touchpads can follow based on hardware coverage.

Reuse the [LinuxKPI I²C transfer core](../kernel/linuxkpi/src/i2c.c) and the [native evdev implementation](../kernel/src/drivers/input/evdev.c). Import drivers from the pinned Linux tree using the existing [LinuxKPI import workflow](linuxkpi.md). Put compatibility code in LinuxKPI or a native bridge.

## First hardware target

The first physical bring-up targets the Synaptics TM3336-001 on AMD Stoney Ridge. The supplied Linux baseline identifies it as HID over I²C with ACPI ID `SYNA2B4E`, HID vendor/product `06CB:8251`, and controller instance `AMD0010:00`. Linux exposed it as `/dev/input/event8` in that boot. Use the firmware and HID IDs for matching and let evdev allocate the node.

## Ordered implementation chunks

T0 fixes the initial hardware and Linux version. T1 can begin alongside T0. T2 and T3 can overlap after target selection. T4 joins evdev and controller bring-up, and T5 qualifies the release.

### T0 — Establish the hardware baseline

[Phase checklist and evidence](touchpad/t0-audit.md)

**Exit:** Record the Linux baseline for each target, confirm the first machine’s firmware and driver path, and save the version lock, compile/link results, and dependency ledger.

### T1 — Add touchpad support to native evdev

[Phase checklist and evidence](touchpad/t1-evdev.md)

**Exit:** The desktop recognizes a synthetic touchpad, handles click and two-finger scroll, and recovers slot state after queue overflow. [Linux multitouch protocol](https://kernel.org/doc/html/latest/input/multi-touch-protocol.html)

### T2 — Discover the controller and client through ACPI

[Phase checklist and evidence](touchpad/t2-acpi-irq.md)

**Exit:** AvoryOS logs a firmware-derived controller, client address, descriptor register, and interrupt route that agree with the Linux baseline. Missing providers produce a clear diagnostic. [Linux ACPI enumeration](https://kernel.org/doc/html/v6.6/firmware-guide/acpi/enumeration.html)

### T3 — Bring up the controller and LinuxKPI client binding

[Phase checklist and evidence](touchpad/t3-controller-binding.md)

**Exit:** Read a valid HID descriptor from the enumerated client on the first laptop. A failed transfer returns a bounded error, removing the adapter releases its clients and IRQs, and graphics DDC/EDID still works.

### T4 — Deliver input from the first physical trackpad

[Phase checklist and evidence](touchpad/t4-hid-input.md)

**Exit:** On the first laptop, a cold boot yields pointer motion, physical click, tap, and two-finger scroll where supported by the device. Reset during active contact recovers without stuck touches or buttons.

### T5 — Expand hardware coverage and validate lifecycle

[Phase checklist and evidence](touchpad/t5-release-qualification.md)

**Release gate:** All three target laptops work through firmware discovery. The matrix records working gestures, cold boot, and suspend/resume status for each model. Hardware and kernel configurations without validated resume are listed as such.

## Evidence and rollback

Use the linked phase files for implementation checklists and phase reports. Record fixture and physical results separately, link logs, and track blocked exit criteria. Add a checkpoint to [LinuxKPI progress](linuxkpi-progress.md) at each completed phase. During bring-up, keep experimental binding disabled by default and use the boot gate to recover the normal input path.

**First implementation deliverable:** T0 audit, version lock, and dependency ledger.

## Dependencies and estimate

Start the hardware inventory and evdev work in parallel. Firmware resource resolution and controller integration can overlap after the first target is chosen. End-to-end hardware input depends on both paths. Use the first working laptop to estimate the cost of each additional controller and GPIO family.

Allow roughly **3–5 months for one kernel engineer** with access to the three target machines. Revisit the estimate after the AML interpreter and first interrupt route are demonstrated. Firmware integration, GPIO support, and system power management are the largest variables.
