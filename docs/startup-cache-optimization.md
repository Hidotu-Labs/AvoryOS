# Startup: retain reclaimable executable pages

The first startup optimization changes `reclaim_unmapped_file_cache()` in
`kernel/src/mm/vmm_free.c`. Previously every completed address-space teardown
called `vfs_cache_clear_unused()` for each mapped file, even with abundant
free memory. Copy-on-write data pages leave their original clean source page
unmapped; the exit/exec cleanup consequently evicted those original pages from
the executable, interpreter and libc caches. Repeated launches then read them
from storage again.

Teardown now retains these reachable cache pages while at least 2048 pages
(8 MiB on x86-64) are available, matching the existing demand-fault pressure
watermark. Below that watermark it still performs eager cleanup. The existing
global file-cache reclaimer also remains able to reclaim them. PTE ownership,
private copies, dirty-page handling and write invalidation are unchanged.

The comparison uses the same unmodified `avory_bench startup 100` executable
in a private KVM guest with four vCPUs, 1 GiB RAM and ext4. Each boot runs six
batches of 100 launches. The first batch is retained in the logs but excluded
from the median of warm batch averages. The final harness also waits 200 ms
after each batch before recording MemFree and Cached, outside the measured
interval. The desktop disk is never attached.

| Warm batch | Before (us) | After (us) |
| --- | ---: | ---: |
| 1 | 575.278 | 204.532 |
| 2 | 575.490 | 202.776 |
| 3 | 581.849 | 198.714 |
| 4 | 569.923 | 195.440 |
| 5 | 620.387 | 193.649 |
| Median | 575.490 | 198.714 |

This is 65.5% less warm fork-to-main time (2.90x throughput equivalent).
Cached memory stabilizes at 2088 KiB after the change, versus 52 KiB before,
with no further cache growth in batches 2 through 5. A first comparison
without settling intervals measured 621.181 us before and 190.166 us after;
the full userland suite measured 216.580 us across its 50 startup launches.
These are isolated-guest results, not a prediction of an exact desktop time,
and do not establish a cold-cache startup improvement.

Temporary syscall/page-fault tracing found roughly 62 page faults per launch;
file-cache misses repeatedly named the same private-data source offsets in
`ld-linux-x86-64.so.2`, `libc.so.6`, and the benchmark. Kernel ELF registration
itself averaged about 3 us. The tracing code was removed after diagnosis.

Verification:

- `scripts/tests/test_startup_bench_boot.py`: six startup batches, existing
  TSC counters and settled memory snapshots; run with `AVORY_STARTUP_RUN=name`
  and optionally `AVORY_TEST_KERNEL=/path/to/kernel` for separate comparisons.
- `scripts/tests/test_global_cache_reclaim.py`: actual exit reclaimer retains
  warm pages at/above the watermark, reclaims below it, and preserves existing
  dirty/loading/writeback/mapped-page and reference-ownership assertions.
  The old implementation fails the new retention assertion.
- `scripts/tests/test_memory_pressure_cleanup.py`: pressure cleanup and retry,
  masked-IRQ behavior and release of all collected file pins pass.
- `scripts/tests/test_file_cache_cow.py`: private cache pages remain CoW across
  racing mprotect; the fixture now follows the current locked mapping API and
  covers already-present leaves and partial ELF tails.
- `scripts/tests/test_page_cache_batch_flush.py`: dirty/writeback behavior passes.
- `scripts/tests/test_userland_benchmarks_boot.py`: complete userland suite,
  process/thread CPU accounting and RAM growth/release pass with the change.

Raw logs are in `build/benchmark-results/startup-baseline-final.log`,
`startup-optimized-final.log`, and `startup-full-suite.log`. The saved baseline
kernel is `build/benchmark-results/kernel-before-startup`.
