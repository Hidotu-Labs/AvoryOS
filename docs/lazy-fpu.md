# Lazy FPU restore

The scheduler saves state only when the departing CPU has an FPU owner. It
then restores architectural defaults to clear that owner's registers, clears
ownership, and sets CR0.TS. The arriving thread restores its buffer on its
first #NM. Once loaded, further FPU instructions run normally until the next
switch. A switch between threads with no live FPU state only checks the local
owner pointer: no XSAVE/FXSAVE, XRSTOR/FXRSTOR, or CR0 write.

The save is deliberately eager for live owners. An off-CPU thread's memory
image is always authoritative, so dispatch on another CPU and reaping need no
remote FPU rendezvous. Scrubbing before dispatch avoids retaining another
thread's register contents behind TS, the mechanism implicated in
[Intel's LazyFP advisory](https://www.intel.com/content/www/us/en/security-center/advisory/intel-sa-00145.html).
This costs an extra initialization restore when an FPU owner switches out.
FPU-heavy workloads can be slower than eager switching because they also take
#NM on each dispatch; the optimization benefits intervals without FPU use.
Libc may use SIMD even when application code uses only integers, so desktop
savings require measurement rather than classifying programs by purpose.

New thread images use deterministic x87/MXCSR defaults and zero vector state,
without executing FNINIT or capturing the creator's registers. Fork, clone,
and vfork snapshot the parent's live state before enqueue; successful exec
resets it. Signal save/return loads the current owner through the existing
hook. Kernel FPU sections load the interrupted thread before saving their
scratch image. BSP and AP startup initialize and arm TS locally after GS/IDT
are available. The existing XSAVE mask covers enabled x87/SSE/AVX/AVX-512
components; CPUs without XSAVE use FXSAVE. XFD is not enabled: CR0.TS handles
first use of the entire enabled state, including baseline x87/SSE.

## Verification

```
python3 scripts/tests/test_lazy_fpu.py
make -C kernel -j4
make userland/test_lazy_fpu.elf
```

The host regression executes the actual ownership functions with mocked FPU
instructions. It checks 10,000 integer switches with zero saves/restores,
first-use restore, same-owner access, register scrub, fork snapshots,
thread teardown, cross-CPU dispatch, exec reset, and IRQ exclusion.

The userland test checks exact x87/SSE and optionally AVX sentinels across
thousands of yields with competing threads, repeated thread creation/exit,
fork, and signal return. It runs correctness across available CPUs, then
pins the benchmark and its threads to one allowed CPU. Integer and SIMD
futex ping-pong loops use raw syscalls to avoid incidental libc SIMD inside
the timed interval. The integer loop contains no floating-point initialization
or computation. Timing uses CLOCK_MONOTONIC; the result is ns per handoff,
including futex and scheduler overhead, rather than an isolated switch cost.

Both two-CPU QEMU TCG configurations (`-cpu max` with XSAVE/AVX and
`-cpu qemu64` with FXSAVE) passed the guest correctness checks and completed
the benchmark. The LinuxKPI kernel-FPU boot self-test also passed. AVX-512
hardware behavior and bare-metal/KVM performance remain unverified.

For performance comparison, run `/bin/test_lazy_fpu` repeatedly on identical
eager and lazy builds with the same CPU, clock, load, and virtual machine
settings. Compare medians separately for the integer and SIMD cases. The
existing disk-image target installs the rebuilt test at that path. TCG
numbers measure emulation and are not evidence of a hardware CPU saving.
