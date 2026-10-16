# GIMP launch: Xorg GPUVM fault, 2026-10-15

The reported failure is not yet fixed. Opening GIMP triggered a GPUVM fault
in Xorg's graphics context, followed by a graphics-ring timeout. The available
log does not establish which BO or page-table update caused it. Do not treat
the diagnostic changes below as a rendering fix.

## Follow-up implementation

The subsequent LinuxKPI audit found and corrected a deterministic defect in
shared red-black tree rotations used by DRM's memory allocator and scheduler.
See [the rotation fix and regression evidence](linuxkpi-rbtree-rotation-fix-2026-10-15.md).
This is a confirmed code defect; its connection to the rare GIMP GPUVM fault
still needs hardware confirmation.

## Evidence

The pasted log matches
`build/logs/prime-runs/20261015T144654Z-siq7aq7w/serial.log`.
The first reported fault belongs to `Xorg:cs0`, PID 131, PASID 32768,
VMID 1, on gfxhub. Its address is `0x0000800105801000`, with neighboring
pages faulting afterward. Status `0x00141051` decodes to TCP, permission
faults `0x5`, walker error 0 and mapping error 0. Zero-status follow-up
interrupts do not establish separate fault causes.

The graphics queue then times out at signaled sequence 182643, emitted
182646. GPU reset starts afterward. PSP ring creation and SMU resume succeed
in the pasted recovery excerpt, which ends before recovery is complete.
The earlier `bwrap` namespace rejection is a separate unsupported syscall
path; temporal proximity does not establish it caused the GPUVM fault.
The full serial log already reports Raphael GPU cache sync enabled on the
GFX and compute rings. Enabling that policy again is not a new fix.

## Diagnostic change

The existing mode 3 stops resource recording after its first 1024 events.
It can exhaust its budget before an intermittent GIMP launch fault, even
though ordinary ioctl metadata continues to roll. Mode 4 instead retains
the last 1024 resource events and last 2048 combined events in fixed storage.
It records a VM_FAILURE marker and freezes atomically with respect to other
recorder producers when the driver's existing gfxhub fault or job-timeout
report obtains task information. The marker contains the device identity,
PASID, process ID and thread ID. The fault address/status remain in the
adjacent upstream serial log. Reset traffic cannot overwrite the frozen trace.

Only the `gmc_v10_0.c` fault reporter and `amdgpu_job.c` timeout reporter
redirect their task-info calls through the wrapper. The actual task lookup
and fault/reset behavior are preserved. The hook allocates nothing, accesses
no hardware and does not dump or wait in interrupt context. Normal mode 1
and bounded mode 3 retain their existing behavior. Mode 4 is opt-in and
changes timing; a successful launch under tracing cannot certify a fix.

## Next reproduction

Boot the rebuilt kernel in the usual user-controlled PRIME session. In a
root guest terminal, arm the recorder immediately before opening GIMP:

```sh
echo 4 > /proc/gpu_debug
```

Open GIMP normally as the desktop user. After the fault, if the guest console
is usable, save and export the already frozen capture:

```sh
echo freeze > /proc/gpu_debug
cat /proc/gpu_debug > /tmp/gimp-gpuvm.log
echo dump > /proc/gpu_debug
```

Alternatively, the existing capture helper accepts mode 4:

```sh
sh /path/to/gpu-debug-capture.sh 5 60 4 /tmp/gimp-gpuvm.log
```

Open GIMP after its five-second arming delay and within the 60-second window.
The hook freezes automatically on a reported fault or unrecovered timeout;
the helper exports the retained report at the end of the window. If no fault
occurs, it freezes the rolling capture at the end instead. Preserve both the
capture and that boot's full host serial log. A fully wedged guest cannot
export its in-memory recorder through this mechanism.

This work does not start or stop a passed-through GPU, edit the main disk,
change Xorg/Mesa defaults or exercise physical reset/power paths.

## Validation

Host tests pass for the actual flight recorder (including concurrent resource
producers racing the freeze, rolling retention, single-trigger preservation,
mode 3 budget compatibility, chunked reads and bounded serial export), the
actual AMDGPU wrappers under ASan/UBSan, the ioctl tracing bridge, the existing
IB visibility policy and the existing VRAM-clearing policy. The changed shell
helper passes `sh -n`. Kernel link and ISO packaging pass; build logs are
`build/logs/gimp-gpuvm-capture-build.log` and
`build/logs/gimp-gpuvm-capture-iso.log`. Object-symbol inspection confirms the
fault/timeout callers use the wrapper and the wrapper calls the original lookup.
No passed-through GPU reproduction was run.

The local Xerj refresh was attempted before investigation and after these
changes. It fails on a pre-existing retained-file family mismatch for
`docs/amdgpu-testing.md`; source investigation used bounded direct reads instead.
