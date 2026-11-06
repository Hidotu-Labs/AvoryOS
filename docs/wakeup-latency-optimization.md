# Loaded wakeup latency

The scheduler now remembers CPU time across runtime-accounting updates,
arms the timer for the remaining request, and gives short blocking bursts
smaller default requests. This reduces contention delays for default-policy
interactive tasks without changing their nice weights.

`eevfd_update_curr()` updates `exec_start_ns` every time it accounts runtime.
Using that timestamp as the dispatch start made a timer/accounting update
reset the minimum preemption floor. The runqueue now records cumulative
runtime at dispatch; elapsed dispatch runtime includes the unaccounted tail.
Timer arming also considers both the remaining physical dispatch budget and
the remaining virtual request, so it cannot repeatedly grant another full
slice to a partially served request. Uncontended timer cadence is preserved.

Those accounting corrections alone changed median headless loaded-wakeup
p99 only from 6084 to 6040 us. The larger improvement requires short-burst
requests: on blocking sleep, default-policy entities predict the next request
as four times their measured CPU burst, bounded by the existing 500 us minimum
and their normal nice-based request size. CPU consumption across preemptions
counts toward the burst. A task that becomes CPU-bound returns to its normal
request size. An unsigned saturating microsecond counter occupies existing
entity padding; the entity remains 96 bytes, preserving thread assembly offsets.

Weights, explicit/custom slices, batch/idle/RT requests, and active kernel
priority ceilings retain their existing behavior. The logic depends on actual
blocking and CPU use; it does not recognize process names or benchmark modes.
It does not lower the global quantum or alter the timer's millisecond precision.

## Measurements

See [measured results](../build/benchmark-results/wakeup-comparison.md) and its
raw logs. The benchmark executable was unchanged. For three 1000-sample batches
in a private headless KVM guest, the median batch loaded-wakeup p99 fell from
**6.084 to 3.003 ms (50.6%)**. In a fresh paired full graphical guest run,
it fell from **6.939 to 2.979 ms (57.1%)**. Graphical loaded pipe/socket p99
fell from approximately 9.1 ms to 1.03 ms.

Idle wakeup did not improve: headless p99 increased from 1.021 to 1.374 ms;
the graphical pair was similar at 1.411 versus 1.432 ms. These measurements
include VM/host scheduling noise and are not a guarantee of every workload's
tail latency. Native CachyOS's earlier 0.654 ms loaded-wakeup p99 remains lower.

## Verification and reproduction

Build the configured kernel before running these checks. Private guest tests
clone/create test disks and never modify the normal disk image.

```sh
python3 scripts/tests/test_sched_dispatch_budget.py
ASAN_OPTIONS=detect_leaks=0 python3 scripts/tests/test_sched_priority_ceiling.py
ASAN_OPTIONS=detect_leaks=0 python3 scripts/tests/test_sched_deadline_tree.py
python3 scripts/tests/test_sched_local_wake_ipi.py
python3 scripts/tests/test_scheduler_wake_publication.py
python3 scripts/tests/test_sched_fairness_boot.py
python3 scripts/tests/test_sched_attr_boot.py
python3 scripts/tests/test_userland_benchmarks_boot.py
AVORY_WAKE_RUN=adaptive python3 scripts/tests/test_wakeup_bench_boot.py
python3 scripts/tests/run_desktop_graphical_benchmark.py --scheduling --samples 1000
```

The dispatch test compiles the actual scheduler code under ASan/UBSan and
checks dispatch accounting, pending runtime, redispatch, remaining request,
sleeper timer precedence, burst bounds, nice values, custom requests and
priority ceilings. Existing tree, local wake notification and wake publication
checks cover related scheduler invariants. The native-ABI fairness boot test
pins two workers to one CPU for three seconds per case: equal nice, a former
sleeper becoming CPU-bound, and nice 0 competing with nice 10. Measured CPU
ratios were 1.004, 1.009, and 0.105, respectively (nice-10 weight predicts
approximately 0.107). It verifies both CPU utilization and runtime share.
The attribute boot test covers policy permissions/custom slices; the userland
suite covers actual workloads plus sleep/thread/process CPU accounting.

For the baseline, set `AVORY_TEST_KERNEL` to the saved pre-change kernel
(`build/benchmark-results/kernel-before-wakeup`) and give headless runs a
separate `AVORY_WAKE_RUN` name. Save graphical result logs between runs:
the graphical runner reuses its private output directory. Measure each kernel
without simultaneous build/test load. The normal ISO was rebuilt with its
existing command line; the disk image needs no update for this kernel change.
