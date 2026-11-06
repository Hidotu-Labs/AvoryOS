# Desktop latency optimizations

This pass targets wakeups under CPU/memory load, synthetic X11 event delivery,
query-instrumented GPU completion, and first touch of a 32 MiB anonymous mapping.
The benchmark workloads and sample counts are unchanged.

## Scheduler and timer

The timer preemption path now checks the earliest eligible virtual deadline,
rather than only the leftmost task by virtual runtime. Its outer execution
floor permits the existing interactive preemption rule to make the decision.
The lookup does not advance virtual time to make an ineligible task runnable.

The runqueue's minimum virtual runtime includes the currently running entity,
which is absent from its red-black tree. Dispatch publishes the arriving entity
as current before removing it from the tree. Enqueue/dequeue cannot advance
the floor past that entity or move the floor backwards. Previously, removing
the leftmost entity could advance the floor to a waiting neighbour and delay
subsequent interactive wakeups.

LAPIC count conversion subtracts the precise current time from an absolute
millisecond deadline. Subtracting the rounded-down current millisecond added
up to another millisecond to each arm. The new conversion rounds upward to
hardware ticks, saturates long waits, and promptly retries overdue deadlines.
Sleep queues retain their existing millisecond resolution. Nice weights,
explicit request sizes, priority ceilings, and atomic-section restrictions
remain in force.

## Anonymous first touch

Sequential anonymous faults already allocate up to 64 ordinary pages. When
the pre-zeroed pool runs out, the fault path now zeroes the uninitialized
suffix in one vector batch under the existing kernel FPU guard. The scalar
non-temporal batch handles CPUs without the vector features. Pool hits are
not cleared twice. The batch's store fence completes before any PTE is
published. Revalidation, sparse/NUMA/locked mapping exclusions, allocation
ownership, and low-memory fallback are retained.

## Virgl query notifications

The renderer's asynchronous fence worker signals its event fd when it needs
the GL/main thread to check pending queries before retiring a completed
fence. QEMU previously serviced this through its 10 ms fallback timer. The
local patch registers the fd with QEMU's main loop, preserving query-before-
fence ordering and asynchronous fence callbacks. It unregisters the fd before
renderer cleanup. The timer remains as a fallback; there is no busy polling
or early fence signalling.

`scripts/build-qemu-host.py` builds checksum-pinned QEMU 11.1.2 with
`scripts/qemu/virgl-query-event-poll.patch`, installs the executable atomically
under `build/qemu-host`, and supplies its bundled firmware locally. It never
replaces the system QEMU. Normal `make run` uses it for the default guarded
Virgl path. `RUN_VIRGL_EVENT_POLL=0` selects system QEMU; `RUN_VIRGL=0` and
`RUN_VIRGL_HOST_FIX=0` also preserve their system-QEMU paths.

The private graphical runner uses this local executable when present.
`AVORY_TEST_QEMU=/absolute/path/to/qemu-system-x86_64` overrides the selection,
and `AVORY_TEST_KERNEL` selects a preserved kernel. Each graphical run records
its selected executable in `qemu-command.txt`. No normal guest disk is used
by the private runners.

## Validation and results

See [the measured comparison](../build/benchmark-results/desktop-latency-pass.md)
for batch values, controls, and limitations. Test coverage includes actual
EEVFD candidate selection and floor accounting, absolute timer count boundaries,
equal-nice and weighted CPU runtime shares, sparse/zero-fill/fork/permission
checks, and live SIMD state across a page-faulting user store.

```sh
make qemu-host
python3 scripts/tests/test_lapic_absolute_deadline.py
python3 scripts/tests/test_sched_dispatch_budget.py
python3 scripts/tests/test_anon_write_cluster.py
python3 scripts/tests/test_sched_fairness_boot.py
python3 scripts/tests/test_anon_write_cluster_boot.py
AVORY_WAKE_RUN=latency python3 scripts/tests/test_wakeup_bench_boot.py
AVORY_RAM_RUN=latency python3 scripts/tests/test_ram_bench_boot.py
python3 scripts/tests/run_desktop_graphical_benchmark.py --scheduling --repetitions 3
python3 scripts/tests/run_desktop_graphical_benchmark.py --graphics --repetitions 3
```

The graphical runner reuses output filenames; save each trial before another
run. X11 delivery is a synthetic event round trip. Query-instrumented completion
is a separate phase from query-free completion. Neither measures physical
input-to-photon latency, and device timer samples rejected by the benchmark
remain excluded.
