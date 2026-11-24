# T4 — Deliver input from the first physical trackpad

**Status:** Planned.

**Dependency:** T1 evdev and T3 controller binding.

## Implementation

Use the Synaptics TM3336-001 (`06CB:8251`) for the first physical input milestone. Match the reported HID identity and confirm libinput discovers the resulting evdev node.


- Import the Linux HID over I²C transport and the HID and multitouch code required by the pinned drivers. Bridge the Linux input calls these drivers use into native evdev.
- Implement interrupt-driven reads in sleepable context. Check threaded IRQ masking, acknowledgement, and teardown against the target's interrupt mode. Validate descriptor and report lengths and handle device reset, including release of active contacts.
- Confirm that libinput classifies the device correctly and that the same touchpad is exposed only once if firmware presents an alternate interface.

**Exit:** On the first laptop, a cold boot yields pointer motion, physical click, tap, and two-finger scroll where supported by the device. Reset during active contact recovers without stuck touches or buttons.

## Evidence to record

Record HID and evdev traces from the first laptop, libinput behavior, reset recovery, and results with the experimental binding disabled.
