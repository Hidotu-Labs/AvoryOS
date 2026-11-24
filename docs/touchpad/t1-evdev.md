# T1 — Add touchpad support to native evdev

**Status:** Planned.

**Dependency:** Can begin alongside T0.

## Implementation


- Add dynamic device registration with per-device capabilities, input properties, axis ranges and resolution, and whatever device metadata the desktop discovery path needs.
- Track Type B multitouch slots and tracking IDs. Implement the state queries that libinput uses to reconstruct contacts after `SYN_DROPPED`, including `EVIOCGMTSLOTS`.
- Exercise the normal desktop path with synthetic frames before connecting the hardware. Keep tap, scroll, and gesture policy in libinput. Preserve the existing keyboard and mouse nodes and behavior.

**Exit:** The desktop recognizes a synthetic touchpad, handles click and two-finger scroll, and recovers slot state after queue overflow. [Linux multitouch protocol](https://kernel.org/doc/html/latest/input/multi-touch-protocol.html)

## Evidence to record

Record the desktop session, libinput device classification, captured event frames, overflow recovery, and keyboard and mouse regression results.
