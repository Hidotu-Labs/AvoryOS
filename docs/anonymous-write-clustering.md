# Sequential anonymous write faults

Large anonymous allocations previously took one fault per 4 KiB when first
written. The fault handler now recognizes consecutive write faults within
the same mapping and installs batches of zeroed, ordinary 4 KiB pages per
fault. A sustained stream grows from sixteen to thirty-two to sixty-four
pages per fault (a 256 KiB maximum). The initial write still allocates one
page; a gap resets the batch size. The prediction includes
the mapping start, so adjacent mappings do not inherit a sequential streak.

Clustering applies to private anonymous mappings of at least 1 MiB with
default NUMA policy and at least 8 MiB plus the proposed batch free. Shared,
file-backed, driver, stack and locked mappings retain their existing paths.
Reads retain shared-zero-page behavior. Huge-page policy is unchanged.

Allocation happens before taking the address-space and page-table locks.
The handler then validates the mapping again, preserves resident, CoW and
swap entries, and releases any unused allocations. Allocation failure falls
back to the existing demand-fault path. Once sequential access is detected,
up to sixty-three extra pages may become resident ahead of the application.

The installer walks the page-table hierarchy once per leaf-table window,
rather than repeating presence and mapping walks for each page. It writes
only all-zero PTEs, never splits existing huge mappings, and transfers frame
ownership only for installed pages. Fresh entries need no TLB invalidation.
An empty pre-zeroed pool is probed once per batch; remaining pages use the
normal allocate-and-zero path.

The unchanged `avory_bench memory 32` executable runs six times per boot in
a private KVM guest with four vCPUs and 1 GiB RAM. Each run had a 200 ms
settling interval. The first run is excluded from the warm median.

The original sixteen-page optimization measured:

| Warm measurement | Before clustering | Sixteen-page batches | Native CachyOS |
| --- | ---: | ---: | ---: |
| First touch, ms | 7.357 | 4.204 | 2.357 |
| Read/write stream, MiB/s | 30115.509 | 36283.640 | 38982.532 |

First-touch time decreased 42.9% in this paired guest comparison. Streaming
results vary with host/cache state; they do not establish a corresponding
kernel throughput improvement. CachyOS remains faster on this repeated warm
memory test. Native Linux and virtualized AvoryOS are different execution
environments, so these measurements do not establish an overall OS ranking.

The follow-up compares the saved sixteen-page kernel against adaptive
batching plus the single-walk installer. Two sequential baseline/optimized
pairs each contribute five warm samples per kernel:

| Warm first-touch median, ms | Sixteen-page baseline | Adaptive + batch installer |
| --- | ---: | ---: |
| Pair 1 | 4.297 | 3.776 |
| Pair 2 | 4.861 | 4.104 |
| All ten warm samples | 4.496 | 3.940 |

The combined median decreased 12.4%. Streaming did not improve consistently;
this change targets fault handling rather than the userland streaming loop.
The installer alone initially measured 4.129 ms against 4.297 ms, too small
to establish a reliable standalone gain from that run.

Verification:

- `python3 scripts/tests/test_anon_write_cluster.py` extracts the actual
  helper into an ASan/UBSan harness and checks bounds, allocation failure,
  NUMA/pressure/locked/stack fallbacks, mapping races, adaptive growth/reset,
  bounded tails, partial zero-pool hits and PTE preservation.
- `python3 scripts/tests/test_fresh_page_batch.py` extracts the actual PTE
  installer and checks leaf/PD/PDPT boundaries, permission propagation,
  resident/swap/huge preservation, partial OOM progress and user-space bounds.
- `python3 scripts/tests/test_anon_write_cluster_boot.py` checks zero-fill,
  fork isolation, protection changes and sparse mappings in a private guest.
  Two nonconsecutive writes leave exactly two pages resident via `mincore`.
- `python3 scripts/tests/test_pmm_references.py` and
  `python3 scripts/tests/test_lazy_fork.py` cover ownership and CoW behavior.
- `AVORY_RAM_RUN=name python3 scripts/tests/test_ram_bench_boot.py` repeats
  the RAM measurement; `AVORY_TEST_KERNEL` can select the saved baseline.
- The full userland boot suite exercises all benchmark categories, process
  and thread CPU accounting, and RAM growth/release.

Raw logs are under `build/benchmark-results/`: `ram-baseline-final.log`,
`ram-optimized-final.log`, `ram-cachyos-repeated.log`,
`ram-cluster-correctness-final.log` and `ram-optimized-full-suite.log`.
The baseline kernel is `kernel-before-ram` in that directory.

Follow-up logs: `pte-baseline-ram-clean.log`, `adaptive-pte-ram.log`,
`pte-baseline-repeat.log`, `adaptive-pte-repeat.log`,
`adaptive-pte-correctness.log` and `adaptive-pte-full-suite.log`.
The follow-up baseline kernel is `kernel-before-pte-batch`.
