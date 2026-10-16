# VCN startup timeout on Raphael passthrough

The user confirmed the target is `ring vcn_dec_0 test failed (-110)`.
The latest failed capture is launch `20261016T084737Z-nska174_`:
MODE2 returns zero, PSP ring replacement succeeds, and all traced PSP firmware
commands complete with status zero, including the VCN image and the later DPG
SRAM update. The decode-ring scratch-register test subsequently times out.
These observations do not prove VCN firmware began executing.

Earlier launches in this same host boot ID include both success and failure;
`20261016T083217Z-euktt3ol` reports successful decode/encode initialization in
DPG mode. The user does not know whether the trigger is first launch, reopening,
or host resume. No matching suspend event was found in the readable current
host kernel journal. Lack of a matching record is not proof there was no suspend.

The imported `amdgpu_vcn_dec_ring_test_ring` writes its scratch seed, allocates
and commits two command dwords (plus padding), then polls for the ring-written
value for `adev->usec_timeout`. `amdgpu_ring_alloc` invokes a void begin-use
callback; VCN's callback requests power-up but does not propagate its return
value to ring allocation. The generic power-up dispatcher can log errors, so
absence of an error must not be treated as a successful hardware transition.

## Diagnostic candidate

Wrappers in `kernel/linuxkpi/src/amdgpu_trace.c` preserve the original calls,
arguments and results. They add only software-field logging:

- decode test entry: ring address, byte size, doorbell selection/index, write
  pointer, cached power state and existing timeout;
- VCN power requests: requested and cached before/after state, DPM enable,
  indirect SRAM and power-gating flags, and the dispatcher's exact result;
- decode test exit: resulting write pointer, cached power state and result.

Power traces are capped at 16 per kernel boot. Logging is scoped to hardware
access on the 1002:164e APU passthrough PF. There are no extra MMIO reads or
writes, firmware commands, resets, retries, timeout changes or disabled media
engines. Ring-test wrappers do not convert errors to success. This is evidence
collection, not a demonstrated startup repair. The withdrawn media-reinit
experiment remains removed.

## Validation and hardware boundary

`test_vcn_startup_trace.py` executes the actual wrappers under ASan/UBSan with
modeled original functions. It checks scope, log cap, single delegation,
unchanged error/success returns and write-pointer results. It cannot establish
hardware accessibility, firmware state or successful startup. The existing
partial-init cleanup wait-macro regression also passes.

Before a physical trial, preserve current work: earlier passthrough trials
recorded host Data Fabric sync-flood resets (see `amdgpu-host-reboot-2026-10-15.md`).
A software-only passing test does not establish that launching the physical
GPU is safe. No new physical initialization, host suspend/reboot, bridge reset
or firmware recovery sequence has been run for this candidate.

The complete kernel build passes. Symbol inspection confirms both original
functions and their wrappers are linked. `build/vcn-startup-20261016/candidate.iso`
boots in an isolated eight-vCPU VM; native signal/restart ABI and alternate-stack
regressions both exit zero and the guest powers off normally. The matching debug
ELF and hashes are saved beside it. The current desktop ISO and disk have not
been modified. Approval for a physical GPU startup trial is pending because of
the previously observed host-reset risk.

The user chose to launch the physical test themselves. After checking that the
current ISO was unchanged since candidate preparation and had no open file
descriptors, the candidate was installed as `avoryos-x86_64.iso`. Rollback is
`build/vcn-startup-20261016/iso-before-vcn-trace.iso`. The agent did not launch
the physical GPU. The normal `make run-c7-prime` launch records the added VCN
lines automatically in `build/logs/p6-c7-prime.log` and its per-launch copy.
