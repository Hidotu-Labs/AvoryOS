# Firefox / YouTube tab reopening on RTL8139, 2026-10-28

The requested sequence is: start Firefox in KDE Plasma 6, open YouTube,
play a video for 10–20 seconds, create another tab, close the playing tab,
then open YouTube again. All verification runs for this investigation use
KDE Plasma 6 and QEMU's RTL8139 emulation, eight vCPUs and 4 GiB RAM.

## Captured causes

### Interrupt completion before signal delivery

`isr_dispatch()` delivered user signals before acknowledging the interrupt.
A fatal signal can terminate the interrupted task and never return to that
interrupt frame. The APIC then retains the device interrupt's in-service bit.

In `firefox-youtube-rtl8139-plasma-baseline2`, CPU 1 was halted in the ordinary
idle loop with IF enabled. Its APIC nevertheless had vector 102 (NVMe's
CPU-1 completion IRQ) in service, PPR 0x60, and vectors 48, 49, 50, 96 and 97
pending. Timer, reschedule, TLB and deferred-free IPIs could not be delivered.
CPU 3 waited for a TLB acknowledgment and CPU 7 waited for a VFS grace period.
The saved debugger and APIC dumps establish an interrupt-completion failure;
an idle thread alone would not identify it.

The common return path now acknowledges the device interrupt, exits hardirq
accounting, and then delivers signals. Timer vector 48 retains its existing
early acknowledgment. A terminating or stopping signal can abandon its frame
after completion without blocking the CPU's later interrupts.

### Memory-pressure queries monopolizing the allocator

`zram_under_pressure()` checks free memory on user page faults. Previously,
`pmm_get_free_pages()` obtained each buddy-zone lock and validated and walked
all its free lists. Browser allocation and tab teardown fragment those lists,
so every fault could repeat a long walk while excluding allocations and frees.

The comparison kernel containing only the interrupt fix reproduced the slow
reopen: after verified 15-second 1080p playback and tab closure, YouTube's
homepage failed to become ready within 60 seconds. With lightweight monitoring,
the debugger caught CPU 0 in `buddy_zone_free_pages()` and CPUs 2, 5 and 6
spinning on the same zone lock. TLB and VFS acknowledgment arrays were clear.
This isolates allocator contention from the interrupt-completion hang.

Buddy zones now maintain per-order block counts and a published free-page
total. Pressure queries use one atomic load per zone. Boot seeding initializes
the counts while preserving the original free-block layout. Splits and merges
update private accounting; the total is published at complete allocation/free
boundaries. Readers cannot mistake the temporary removal of a large block
during splitting for a sudden shortage of memory. Corrupt-list recovery also
adjusts the counts when it discards a list or tail.

## Regression checks

- `test_irq_signal_eoi.py` executes the actual dispatch/return functions with
  a signal-delivery mock that abandons the stack. It covers device IRQs,
  unregistered IRQs, the timer, exceptions, kernel/user returns and int 0x80.
  The original source fails the acknowledgment assertion.
- `test_buddy_free_accounting.py` compiles the actual allocator and boot seed
  fragment. ASan/UBSan checks cover seeding, allocation, splitting, merging,
  constrained allocations, PCP refill, fragmentation and corrupt-list repair.
  Concurrent pressure reads remain between the real allocation bounds and
  take neither a lock nor a free-list walk. LeakSanitizer is disabled for the
  container's ptrace restriction. The original source fails the lock-free
  query assertion.
- Existing IRQ preemption and PMM reference/PCP regressions pass. The PMM test
  shim was updated for the current batch-zeroing/FPU entry points.
- The kernel builds and links. Existing warnings include the RWX LOAD segment.

## Browser verification

`scripts/tests/test_firefox_youtube_reopen_boot.py` always starts Plasma and
records the installed plasmashell ELF's version literal after shutdown. It verifies advancing decoded
frames and at least ten seconds of playback during each 15-second observation,
then repeats the requested tab sequence three times. The final playing tab is
closed while Firefox remains open, followed by CPU-recovery sampling.

The probe uses a clean Firefox profile with the default content-sandbox
configuration. Its private Firefox archive changes only Marionette's listener
binding, because guest localhost routing is unavailable. QEMU's host forward
binds to 127.0.0.1. The desktop disk is read to prepare a private image; QEMU
attaches the private copy.

Page readiness requires a completed document and YouTube's search input; an
empty recommendation feed is valid. Timing includes loading that document.
The probe rejects reopening above 30 seconds, absent playback, sustained final
CPU above 25%, kernel faults, allocator corruption, and hang reports.

The final kernel completed the sequence in
`build/firefox-youtube-rtl8139-plasma-final/`. The installed plasmashell version
is 6.8.1; Firefox is 158.0.1, running graphically with VirGL and audio.

| Cycle | Reopened YouTube document ready | Reopened video |
| --- | ---: | --- |
| 1 | 2.128 s | 15-second observation, 1920×1080, advancing decoded frames |
| 2 | 2.029 s | 15-second observation, 1920×1080, advancing decoded frames |
| 3 | 4.292 s | 15-second observation, 1920×1080, advancing decoded frames |

The last playing tab was closed 116.963 seconds after the host driver started.
Firefox stayed open on `about:blank`. The last sampled 30-second CPU window
before browser termination averaged **0.10% busy across all eight CPUs**.
The native monitor completed, QEMU shut down, and the serial log contains no
kernel fault, allocator-corruption or hang-report marker.

The initial postprocessor expected a pacman package-version record, but this
desktop image omits pacman's package database. The retained VM artifacts were
revalidated using the installed ELF version literal and the corrected
`--verify-existing` checks. This did not rerun or alter the browser workload.
The final validation log is
`build/youtube-rtl8139-race-investigation/final-browser-validation.log`.

Reproduction and retained-evidence validation:

```sh
python3 scripts/tests/test_firefox_youtube_reopen_boot.py --plasma --cycles 3
python3 scripts/tests/test_firefox_youtube_reopen_boot.py --cycles 3 \
  --verify-existing build/firefox-youtube-rtl8139-plasma-final
```

## Installed image

`avoryos-x86_64.iso` contains the tested kernel. Its extracted kernel matches
the kernel in the passing private VM's ISO byte-for-byte. Limine configuration
was also compared byte-for-byte before atomic installation. The production
desktop disk was not changed.

- ISO SHA-256: `4ddc590291dd760e9c180ba106e7708fa1f83509d3530bbf82865319eb0381ab`
- Kernel ELF SHA-256: `159e6887a7ef86bbaf8046952bf25baa20c9d708be1aef1c5a9dca7f664ba961`
- Rollback: `build/youtube-rtl8139-race-investigation/iso-before-irqfix.iso`
- Installation metadata: `build/youtube-rtl8139-race-investigation/installed.json`

## Failed and limited observations

Earlier full-thread dumps substantially increased monitoring time and are
diagnostics rather than a browser timing comparison. An intermediate allocator
candidate had incomplete boot accounting and was rejected. Another intermediate
run reported a corrupt free-list header; it is not a passing regression run.

Longer uninterrupted playback in intermediate candidates displayed a YouTube
player error after approximately 41 and 54 seconds. The HTTP diagnostic recorded
successful media responses and no HTTP 403/5xx responses; the error's cause is
not established. Those runs do not establish successful extended playback.

A final-candidate attempt also stalled in D-Bus/Firefox application startup
with CPUs idle and no pending TLB/VFS acknowledgments. Its diagnostics are
retained separately from the APIC hang above, and it does not count as a
successful browser run. The version check in those attempts launched an extra
Qt application before Firefox; version inspection now reads the installed ELF
after shutdown without starting another desktop application.

Evidence, comparison kernels and the rollback ISO are under
`build/youtube-rtl8139-race-investigation/`.
