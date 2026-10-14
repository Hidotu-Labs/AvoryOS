# Raphael abrupt-exit recovery: combined candidate

## Failed stop-only trial

`build/amdgpu-xclose/trial-y2bxe1wn` enabled PSP stop-before-create without
MODE2. Ring creation and VCN loading completed, but AUTOLOAD_RLC returned raw
status `ffff000d` and KIQ/KCQ initialization timed out (-110). No interpretation
of that firmware status is assumed. The user subsequently reported a host
reboot; the host boot ID changed from `6bb175cf-6b43-4e4c-9f43-26001fc29d9d`
to `e036d29b-b9fa-4a1f-a468-4822abbc59c0`. This candidate did not establish
successful recovery.

That run also exposed a native boot parser defect: searching for `init=` as a
substring matched `kpi_amdgpu_psp_reinit=1`, selecting executable `1`. The
parser now requires an argument boundary. The actual parser regression passes;
subsequent physical guests executed `/bin/probe-init`.

## Combined MODE2 and PSP stop trial

`build/amdgpu-xclose/trial-6jlx9bmg` performed two physical boots using both
explicit parameters. Each executed the scoped imported MODE2 reset (ret=0),
checked PSP stop acknowledgement, created a new ring, and registered AMDGPU.
The first guest was terminated through GTK WM_DELETE_WINDOW after initialization,
without guest shutdown. The second initialized and completed the AMDGPU shutdown
callback before powering off. Controller result PASS, two physical launches,
unchanged host boot ID. No host bridge/VFIO/power/firmware settings were changed.

The combination addresses two observed boundaries: restoring GFX initialization
and replacing retained PSP rings. These observations do not establish firmware
status meanings or prove that either operation alone is sufficient. Earlier
MODE2-only and PSP-stop-only failures remain part of the evidence.

The Ctrl-C pair in `build/amdgpu-xclose/trial-maeiyumw` also passed. The
launcher forwarded SIGINT to QEMU; the first guest exited without the driver
shutdown callback. The reopened guest initialized and shut down normally.
Both guests performed MODE2 and checked PSP ring replacement; host boot ID
remained unchanged. Four physical boots and two abrupt exits passed so far.

## Automatic scoped policy

Both kernel booleans now default to true. Their device/passthrough/IP checks
remain mandatory. The normal PRIME target enables both, with independent
explicit disable switches. The single-boot baseline runner disables both to
preserve its original comparison. No power-cut, shared bridge reset, host
configuration change or QEMU modification was introduced.

The rebuilt native-default X-close pair in `build/amdgpu-xclose/trial-y0irflhz`
also passed. Its command line omitted both recovery parameters. Both boots
executed the combined recovery and initialized; the second completed orderly
AMDGPU shutdown. The private ISO kernel exactly matches the stripped current
ELF (SHA256 `9cb9013a6511fdaa1e612fed2c08ac89ab1d35ea167d9e91959c770dfcb0615f`).

Total: six physical boots, three abrupt exits (two GTK X-close, one Ctrl-C),
three successful reopens, no host reboot during these three combined trials.
Observed PSP completion statuses were zero. The controller records and serial
logs are retained per trial; consolidated checks are in
`build/logs/combined-recovery-hardware-summary.json`. This is bounded positive
evidence after earlier failed candidates, not a production certification.

The build and eight software regressions passed. `git diff --check` passed.
Xerj refresh was attempted but remains unavailable due to the existing
`docs/amdgpu-testing.md` family mismatch; investigation used bounded source
reads and exact searches instead.

## Limits

CPU ASan/UBSan regressions validate policy, order and error preservation only.
Physical init/reopen checks do not verify pixel corruption, Minecraft stability,
all abrupt termination timings, or production reliability. Imported AMDGPU
sources remain unchanged by these recovery wrappers.
