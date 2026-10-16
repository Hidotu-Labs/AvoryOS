# Host reboot recurrence: 2026-10-15

The user reports the platform reboot recurred. The prior successful close/reopen
pairs did not establish that the host reboot was resolved. Pixel corruption is
reported after the reboot and clears after later initializations; this is a
correlation, not proof that host reboot is its sole cause.

## New retained host evidence

Failed launch: `build/logs/prime-runs/20261015T085632Z-32eldex_`.
Previous boot ID: `e036d29b-b9fa-4a1f-a468-4822abbc59c0`.
Current boot ID: `ef573169-91c6-48a2-a3ec-3ccbf09a8ef5`.
The next host kernel reports:

```
x86/amd: Previous system reset reason [0x08000800]: an uncorrected error caused a data fabric sync flood event
```

The kernel's documented reset-reason interpretation is here:
https://kernel.org/doc/html/next/arch/x86/amd-debugging.html
This identifies a platform error/reset class, not a specific failing GPU,
firmware command, RAM component, or driver instruction. No host hardware defect
is declared from this status. There is no persisted panic record available in
the unprivileged collection; pstore could not be read and systemd-pstore has no
entries. A missing readable record does not prove there was no panic.

The previous host resumed from deep/S3 at 11:55:59 Istanbul; QEMU launched at
11:56:32 (about 33 seconds later). The previous journal ends at 11:56:29 and
the new boot begins at 11:57:03. Resume is a new correlation worth preserving;
it does not prove that suspension caused this failure.

The guest records MODE2 ret0, PSP stop/create success, TMR setup status0 and
AUTOLOAD_RLC fence15 status0. Its tail is VCN LOAD_IP_FW type0xd, GPU0xb82000,
size0x8bb50, fence16/wptr256, with publication returned. This matches a previously
retained failed boundary. It does not prove VCN caused the reset or that its
completion never occurred: the completion hook can miss a sampling race.

Read-only snapshots: `build/logs/host-reboot-20261015/`.
Full collection: `/tmp/avory-host-evidence.sYxd5NDB`; privileged dmesg/ACPI/pstore
reads were unavailable. No new QEMU physical trial, host configuration change,
bridge reset or power operation was performed.

## Media acceleration requirement

The user rejected disabling JPEG acceleration. The uncommitted media-isolation
experiment was removed from the launcher, LinuxKPI policy and tests. VCN/JPEG
remain enabled. No physical trial of that experiment was performed.

The platform reset remains unresolved. Future recovery work must preserve media
acceleration. The retained VCN upload boundary and host reset reason are evidence
for investigation, not proof that disabling a media block is the solution.
