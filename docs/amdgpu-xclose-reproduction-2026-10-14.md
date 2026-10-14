# One approved GTK X-close/reopen hardware sequence

Result: X-close recovery FAILED on the reopened guest. The host did not reboot
in this two-boot sequence. No third physical guest or reset retry was launched.

The user authorized one initialization, X-close, then one reopened initialization.
MODE2 was explicitly disabled in both guests. No host reset method, power,
bridge, binding or firmware settings were changed.

## Controlled procedure

`scripts/amdgpu-xclose-recovery.py --run` prepared a disposable root image and
ISO with the current kernel. The init program holds for 45 seconds after kernel
initialization, then requests orderly power-off if its QEMU process is still
alive. Both boots used the same prepared ISO and root image. There was no
userspace rendering workload; this tests abrupt exit after driver initialization.

The QEMU GTK display ran on an isolated Xvfb X server. Xvfb was extracted into
`build/tools/xvfb` without installing or changing system packages. The close
helper sends the standard WM_PROTOCOLS/WM_DELETE_WINDOW client event to the
named window. It does not destroy the window, send a termination signal, or issue
a QMP quit command itself. GTK's existing close handler handles the event. The
same mechanism was verified first on a GPU-free paused QEMU instance, which
exited normally.

The first physical guest reached `Initialized amdgpu 3.54.0 ...` and userspace.
Its VCN fence 16 matched with raw status 0. Then the helper sent the close event
to window title `raphael-xclose-1`. QEMU exited with status 0 before the 45-second
hold elapsed. The serial log contains no guest power-off request or AMDGPU
shutdown callback. This establishes abrupt termination through the actual GTK
window-close path rather than guest shutdown.

The controller then started exactly one second physical guest. This guest
failed at PSP ring creation:

```text
PSP ring-create enter type=2 gpu=f400210000 size=4096 timeout_us=100000
PSP wait failed reg=16080 expected=80000000 mask=8000ffff
observed-after-failure=80020115 changed=0 ret=-62
PSP ring-create exit ret=-62
hw_init of IP block <psp> failed -22
Fatal error during GPU init
```

The raw status is retained without decoding 0x0115. This failure precedes TMR
setup and VCN firmware publication in the second boot. Retained state at PSP
ring creation is now the immediate investigation target for this reproduction;
it does not establish that an old ring is the sole retained state or that
DESTROY_RINGS is a safe/full recovery operation.

After failed-probe cleanup the second guest reached userspace and performed
orderly power-off. Because the AMDGPU probe failed, it was unbound and did not
receive a shutdown callback. QEMU itself exited with status 0, while the
controller returned failure because GPU initialization had failed. Process exit
status 0 must not be treated as GPU initialization success.

The failed-probe path logged `ttm finalized` and completed all initcalls with
no THREAD FAULT/CR2=0x60. This is one hardware check of the earlier cleanup-order
correction. IRQ and drm_buddy cleanup warnings remain; they are not claimed
fixed. The host boot ID stayed unchanged throughout the sequence.

## Evidence

Trial directory: `build/amdgpu-xclose/trial-kzp7zme_/`:

- `boot1-serial.log`: successful init, then abrupt GTK close without shutdown.
- `boot2-serial.log`: PSP rejection, completed error cleanup and normal power-off.
- `controller.log`: actual WM_DELETE_WINDOW events and launch output.
- `state.json`: exactly two physical launches and second_initialized=false.
- `prepared-command.json`: QEMU device/display configuration.
- `launch-evidence/`: per-boot kernel ISO digests, stderr and serial snapshots.

Controller transcript: `build/logs/amdgpu-xclose-approved-trial.log`.
Machine-readable outcome: `build/logs/amdgpu-xclose-approved-summary.json`.
Read-only host snapshot: `/tmp/avory-host-evidence.Z3BNSZpg`; unprivileged dmesg,
pstore and raw ACPI table reads are explicitly marked unavailable.

No further launch is authorized by this approval. No recovery command was added
or tested. The existing earlier failure after VCN publication and the repeated
host reboot remain separate unresolved observations; this sequence reproduced
the immediate next-boot PSP-ring failure without pursuing repeated retries.
