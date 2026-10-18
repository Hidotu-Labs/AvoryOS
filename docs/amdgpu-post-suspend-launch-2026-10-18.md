# Raphael post-suspend launch prevention

## Firmware recovery candidate prepared later on October 18

The caller after `PSP reinit stop enter` is the imported PSP 13.x ring-stop
callback. For a PF it writes `GFX_CTRL_CMD_ID_DESTROY_RINGS` (0x00030000)
to C2PMSG_64, delays 20 ms, then polls the response flag. The ready read
before dispatch does not establish retained ring/TMR ownership. The last
serial line narrows the suspect window but cannot identify a completed or
failed transaction. No speculative command or status interpretation was added.

Upstream firmware regression evidence offers a separate candidate:
[Debian #1150535](https://bugs.debian.org/cgi-bin/bugreport.cgi?bug=1150535)
reports Raphael resume failures fixed with the 20260916 bundle. It concerns
another system/IP variant and s2idle, not this exact VFIO/deep-sleep reset.
Our staged DMCUB was the reported affected version 0x05003300. The generated
manifest revealed that the staging source was an unpinned /lib/firmware tree.

The build now pins 20260916, verified against upstream tag object
247f5c7b8205a287bf4c48659454fb769928f2f0, commit
ab23307cfe7f9366c819025ca3e4778299bc2db2. All eleven blobs have reviewed SHA256
checksums; a stale plain source is rejected before the staged bundle is
published. Of our former files only DMCUB (0x05003500) and psp_13_0_5_ta.bin
change. VCN, RLC and TOC are byte-identical. Previous blobs and a complete
SHA/version comparison are in build/raphael-firmware-20260916/.

One ordinary physical-device probe with this bundle completed PSP ring stop,
AMDGPU registration, userspace execution and orderly AMDGPU shutdown. The
serial log confirms DMCUB 0x05003500. This is a candidate, not a demonstrated
fix for the host reboot. These firmware payloads are loaded after the suspect
stop boundary, so the update cannot be assumed to fix that command directly;
firmware retained from an earlier guest could still matter across host sleep.

The user authorized one post-resume test and performed host sleep/wake.
The prepared command used for that single trial was:

```sh
python3 scripts/amdgpu-psp-boot.py --run --driver-defaults --post-resume-trial
```

The explicit diagnostic flag permits a resumed-host launch through the logger
and records that exception in launch.json; normal launch checks stay enabled.
The probe uses a private disk, starts one physical guest, and requests orderly
shutdown. If it stalls it leaves QEMU alive. Do not retry the failed firmware
state or interpret a normal initialization as a post-resume test.

Firmware staging/hash rejection, image installation, launch-policy and logging
tests pass; the kernel builds. The authorized post-resume result is recorded below.

## Earlier launch prevention and baseline evidence

The host reboot after resume remains a firmware/hardware recovery problem,
not a validated LinuxKPI recovery fix. The user's latest guest trace ends at
`PSP reinit stop enter prior=80020000`. That is the last emitted log, not proof
of which transaction caused the platform reset. No meaning is inferred from
this raw PSP value.

Read-only host evidence was collected in
`/tmp/avory-host-evidence.QBRRTuF8`. The previous kernel records deep suspend
and resume. The current kernel reports reset reason `0x08000800`, interpreted
by the host kernel as an uncorrected error causing a Data Fabric sync flood.
Privileged dmesg, ACPI tables and pstore were unavailable. The device exposes
bus reset and shares its platform with host-owned audio, CCP and USB functions;
no bridge reset, power change or host suspend was performed.

`scripts/vfio_resume_check.py` reads the selected VFIO device's IDs, the host
successful-suspend counter and the difference between CLOCK_BOOTTIME and
CLOCK_MONOTONIC. For Raphael it rejects a resumed host before QEMU opens the
device. If both history sources are unavailable it rejects the launch. The
clock fallback uses a one-second margin; the suspend counter also detects
short successful suspends. It cannot prevent the host being suspended later
while a VM is already running. Direct QEMU or an explicitly replaced launcher
bypasses the check. This is launch prevention, not device recovery.

The ordinary VFIO make target uses this check. `qemu-prime-log.py` also applies
it for PRIME and the physical diagnostic runners, recording successful
preflight evidence in launch metadata. No guest firmware command, media
acceleration setting or host configuration was changed.

With the user's permission, one disposable 8-vCPU physical-device probe ran
on the freshly rebooted host, using the rebuilt kernel and normal driver
defaults. PSP stop returned `80030000`, AMDGPU registered, userspace ran and
the AMDGPU shutdown callback completed before power-off. Logs are in
`build/amdgpu-psp-boot/physical-serial.log` and its `launch-evidence` directory.
This validates one ordinary initialization; no post-suspend trial was run and
successful post-suspend recovery is not claimed.

## Authorized post-resume result

The user confirmed host sleep and wake, then authorized the prepared single
trial. The host suspend counter was 1 and the launcher's clock evidence
recorded 51.301866128 seconds suspended. The host boot ID remained
5bf49512-8802-4163-ab48-48ccf7b77005 before and after the test.

With the 20260916 bundle, DMCUB 0x05003500 was read, PSP ring stop returned
80030000/ret0, ring creation returned0, AMDGPU registered, userspace ran and
the AMDGPU shutdown callback completed. QEMU exited0 and the host did not
reboot. No PMM corruption/reference errors or thread faults were found in
this attempt's serial slice. Evidence is retained in
build/raphael-firmware-20260916/post-resume-serial.log and
pending-post-resume.json, plus the per-launch metadata.

The stop entry was **prior=80030000**, whereas the reported failure had
**prior=80020000**. The preceding candidate probe shut the GPU down normally.
This passes one post-sleep initialization with an orderly prior shutdown;
it does not validate an unclean retained-ring handoff, identify the cause of
the earlier reset or establish repeated-cycle reliability. No second physical
trial or forced guest close was performed. The normal resume launch guard
remains enabled pending that distinction being resolved.

## Full shutdown and power-on result

After the user reported full host shutdown and power-on, the new boot ID was
4325f541-7fc9-4207-8203-3efee561beda and the suspend counter was0. One authorized
disposable VFIO probe used the verified 20260916 bundle and normal recovery
defaults. PSP stop entered with prior80000000 and returned80030000/ret0; ring
creation returned0, AMDGPU registered, userspace ran and orderly AMDGPU shutdown
completed. QEMU exited0 and the host boot ID remained unchanged. No PMM
corruption/reference errors or thread faults appeared in the attempt's log.
Evidence is in build/raphael-firmware-20260916/power-on-serial.log and
power-on-test.json. The reported prior80020000 failure state was not reproduced.

Software tests cover cold/resumed/unknown history, clock fallback, device
scoping, launch logging, and the existing MODE2/PSP policies. The earlier
withdrawn media-reinit experiment remains withdrawn; VCN/JPEG stay enabled.
