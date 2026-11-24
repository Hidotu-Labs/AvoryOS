# T5 — Expand hardware coverage and validate lifecycle

**Status:** Planned.

**Dependency:** T4 first physical trackpad.

## Implementation


- Bring up the remaining targets using the same discovery and input paths. Add controller and GPIO support where the baseline shows a gap. Tie any device quirks to documented hardware IDs.
- Exercise repeated cold boots and reboots, driver failure cleanup, input ring overflow, and sustained use. Cover keyboard, mouse, USB input, desktop discovery, and graphics DDC as regressions.
- Where system suspend is supported, quiesce report delivery before suspending and restore the controller and device before enabling reports. Verify repeated suspend/resume and publish the result separately for each model.

**Release gate:** All three target laptops work through firmware discovery. The matrix records working gestures, cold boot, and suspend/resume status for each model. Hardware and kernel configurations without validated resume are listed as such.

## Evidence to record

Publish results per laptop, firmware version and controller family. Record repeated boots, resume status, regressions, and the procedure for disabling touchpad binding.
