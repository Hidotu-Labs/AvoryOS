# Withdrawn post-S3 media-reinit experiment

**Withdrawn after hardware failure.** The implementation, launch option and
experiment-specific test were removed. Normal launch is `make run-c7-prime`.
VCN/JPEG remain enabled. The host post-S3 reboot is NOT fixed.

Fresh host boot82c12b65-1ea7-4ae6-8171-6c2ff8f2825b and user attachmentdeae63d0
show MODE2 ret0, successful feature query, then VCN power_status=ffffffff and
drain ret-5. Thus the early register preflight cannot operate at that stage.
No power-state meaning is assigned to the all-ones read. Its error was not
ignored. This experiment wrongly tried to reuse a later hardware-shutdown
sequence before the relevant media access state was established. CPU models
did not establish hardware accessibility or correct sequencing.

Previous ordinary v1 passes did not justify calling the post-S3 path fixed.
Further work needs a supported post-resume handoff/firmware recovery sequence,
not more early MMIO/power-command guesses or repeated firmware-stalled boots.
No physical reset, sleep or initialization trial was run during withdrawal.

The rest of this document preserves the experiment's historical evidence.
Its trial commands and candidate descriptions are obsolete.

---

# Post-S3 media-state reconciliation candidate

Status: revision 1 passed two ordinary hardware initializations, then failed the
post-S3 test. Revision 2 is built and CPU-tested, not hardware-validated. Default OFF.
This does not establish a fix for the host Data Fabric sync-flood reset.
VCN and JPEG remain present and their acceleration/power-up paths are retained.

## Evidence and inferred invariant

The Oct15 failed trace follows S3 resume and ends after VCN firmware publication;
the host's next boot reports reset reason0x08000800. See the recurrence report.
Neither identifies a definitive faulty engine or transaction.

Imported `smu_sw_init()` initializes `smu_power.power_gate.vcn_gated` and
`jpeg_gated` to1. The generic `smu_dpm_set_vcn_enable(false)` and JPEG equivalent
then skip the mailbox callback when those fresh cached values already indicate
gated. These are verified code paths. They are not proof that the media engines
were actually running after S3 or that this assumption caused the reset.

A guest cannot observe the host's S3 lifecycle directly here. This candidate
therefore establishes the expected gated state at each explicitly enabled
matching initialization; it does not guess a host-sleep flag from PSP status.
The normal fresh-boot code assumes engines are gated before PSP firmware loading.

## Implementation

`kernel/linuxkpi/src/amdgpu_media_reinit.c` is scoped to1002:164e APU PF
passthrough, MP0/MP1 13.0.5, hardware access enabled. Before PSP ring stop, creation and firmware loading, it checks the SMU
context and required family callbacks. It queries the running firmware's
feature mask and requires FEATURE_VCN_DPM_BIT, the family callback's prerequisite.
This avoids pretending the generic DPM API is initialized at this early stage.

It then calls the family's existing VCN park callback once, sets the matching
cached state only after success, and does the same for JPEG. The callbacks use
the existing serialized SMU mailbox. No new register protocol or reset is
invented. Errors propagate before PSP ring creation; there is no retry or
power-up rollback against retained buffers. This is a new hardware sequence at
an early boundary and remains experimental despite using existing callbacks.

The subsequent imported media hardware initialization and on-demand power-up
paths remain unchanged. No IP block is excluded. This temporarily parks media
for firmware reload; it does not remove video/JPEG acceleration. Host bridge,
VFIO binding, power configuration and firmware files remain unchanged.

## Prepared trial

```sh
make run-c7-prime KPI_AMDGPU_MEDIA_REINIT=1
```

This enables `kpi_amdgpu_media_reinit=1`. The existing PSP reinit switch must
also remain1 (the normal default); this hook belongs to that checked ring-reinit
path. `KPI_AMDGPU_MEDIA_REINIT=0` is the default and leaves hardware operations
unchanged from the last committed recovery path. No physical trial was launched;
the existing user VM was left running and the host was not suspended.

First establish ordinary initialization and VCN/JPEG usability, then a separately
authorized post-S3 trial. A warm boot alone cannot validate the reported trigger.
No media isolation flag is used; the rejected exclusion experiment remains removed.

## Validation

`test_amdgpu_media_reinit.py` compiles the actual policy, extracted family
callbacks, and generic enable callbacks under ASan/UBSan. The negative control
shows that cached gated1 skips ordinary park commands. The candidate sends
modeled mailbox requests despite that cache. The actual generic/family enable
paths subsequently send both power-up requests and update cached state, proving
no software suppression of later acceleration. Hardware effects remain modeled.

The test also checks default-off, scope/context exclusion, firmware prerequisite,
query and per-engine errors, no later command after failure, and partial-success
cache state. The PSP ring test checks stop/ack before the media hook and prevents
ring creation when that hook fails. MODE2, doorbell teardown and PSP completion
regressions pass. AMD-disabled compilation passes. Kernel/ISO build passes in
`build/logs/media-reinit-build.log`; current stripped ELF matches the ISO kernel.

These CPU results validate contracts, not host safety or successful hardware
quiescence. The candidate may fail its firmware prerequisite or a media command;
record those errors rather than weakening the checks to force initialization.
Xerj refresh failed on the existing document-family mismatch; bounded exact
source reads were used.

## Hardware result and revision2

User's attached trace and retained runs `20261015T091720Z-dqcrdnjj` and
`20261015T091811Z-mnx60k71` show both media commands returning0 and AMDGPU
registration on ordinary boots. Post-S3 run `20261015T091917Z-f31od34e` shows
feature query `2ff33b15befff` ret0, then command0x05/parameter0 timing out with
ret-62. The pinned family PPSMC header defines command5 as PowerDownVcn. JPEG
was not attempted. PSP ring creation and firmware loading were not reached;
upstream reported ring failure and converted the propagated error to-22.
Host boot ID remained ef573169-91c6-48a2-a3ec-3ccbf09a8ef5. No reboot in this
attempt does not establish successful recovery.

Revision1 omitted the existing native shutdown preconditions. The pinned
`vcn_v3_0_stop_dpg_mode` waits for power status1, drains both encode queues and
the decode queue, checks power again and clears UVD_PG_MODE before the native
stop path requests SMU power-down. Its unpause helper relies on software
pause_state and fw_shared; a fresh guest cannot treat these as retained state.
The revision adds the bounded drain/DPG-exit portion as a scoped compatibility
helper for one non-harvested VCN instance. Hardware pause request/ack causes
-EBUSY instead of manipulating an old firmware context. Invalid reads, every
wait failure and a failed mode-clear readback abort before SMU power-down.
There is no forced idle, abandoned queued work, timeout increase or retry.
Non-DPG mode follows the previously tested ordinary path without mode writes.

Media reconciliation now precedes PSP teardown, matching native shutdown's
media-before-PSP order. No JPEG support or block is removed. The original
callback errors are still propagated; the cause of the platform reset remains
unproven. DPG activity was not logged in revision1, so this sequence correction
is a candidate, not proof that a retained DPG bit caused its timeout.

Tests add modeled retained DPG, all five wait failures, paused/inaccessible
hardware, stuck readback, queue-pointer masking and preservation of other power
status bits. PSP tests verify media failure prevents PSP stop/create. Existing
power-up callback tests remain. Kernel/ISO build passes in
`build/logs/media-reinit-dpg-build.log`; the ISO kernel matches the stripped ELF.
No additional physical initialization, host suspend, reset or retry was run.
A previously unacknowledged SMU command may remain outstanding; do not keep
launching the failed state and interpret later failures as a fresh comparison.

## Next attempt remained blocked by the previous command

Attached trace58df0e18 and launch `20261015T092514Z-g5v2c6ob` still use host
boot ID ef573169-91c6-48a2-a3ec-3ccbf09a8ef5. They stop earlier, during MODE2,
with SMU message register0x05/parameter0 and ret-62. There are no revision2
media-drain records. This is not a completed test of revision2's DPG sequence.

The pinned `smu_cmn_send_smc_msg_with_param()` polls the prior response before
writing a new command. On SMU_RESP_NONE it reports the current message and
exits. Here the retained message is still PowerDownVcn, so MODE2 was not sent.
Restarting QEMU did not clear the blocked firmware mailbox in this observed run.
No error was suppressed and no new reset/retry command was added to overwrite
an outstanding transaction. A meaningful comparison requires a fresh host boot
rather than repeatedly launching this stalled state. Host reboot is a test
precondition here, not an implemented guest recovery or a claimed permanent fix.
