# Firefox slowdown investigation, 2026-10-28

The confirmed repair removes an expensive VMA merge path. It does not yet
establish that the user's sustained desktop slowdown has been reproduced.

`sys_mprotect` calls `vma_merge_adjacent` for each bounded protection batch.
Previously, merging even two anonymous regions destroyed and reallocated the
entire process VMA tree, including unrelated file/device mappings. This ran
under the process mm spinlock with interrupts masked. Work and allocations
grew with the number of mappings accumulated by the browser. Allocation
failure during reconstruction could also leave mappings missing.

The merge now keeps surviving nodes, drops only redundant nodes' references,
and reconstructs AVL links, heights and interval maxima directly from the
sorted survivors. It needs one pointer snapshot for a large tree, with no
allocations after mutation starts. Unrelated file/device nodes and their
references remain intact. Existing unrelated working-tree changes were kept.

## Validation

- Actual-source ASan/UBSan tests cover AVL metadata and lookup after repeated
  protection/merge operations, unchanged file/bridge ownership and attributes,
  merging shared bridge references, and allocation failure. The prior code
  fails the allocation-failure regression. LeakSanitizer is disabled in this
  environment.
- A private KVM fixture retains 4,000 mapping intervals and performs 100
  protection cycles, checking original page contents. Four-vCPU elapsed time
  was 41.8 ms before versus 8.7 ms after; a rebuilt comparison kernel with the
  same working sources except the merge implementation took 46.4 ms.
- On one vCPU, elapsed time was 43.4 ms before versus 9.1 ms after; the
  accompanying sleeper's worst wake delay was 43.4 versus 9.1 ms. The repaired
  eight-vCPU case also passed (8.9 ms elapsed).
  Repeating with an explicit monitor-start barrier and the rebuilt comparison
  kernel measured 41.3 versus 9.0 ms elapsed, and 41.3 versus 9.1 ms worst wake.
- The existing Firefox memory-semantic fixture passed on eight vCPUs:
  `MADV_FREE` retains contents, protection changes preserve data, and
  `MADV_DONTNEED` returns zero-filled anonymous pages.
- The existing mmap reservation/driver mapping regression suite passed.
- The kernel compiles and links, with the existing RWX load-segment warning.

## Browser coverage and limits

The browser test clones the desktop filesystem into a private raw image and
boots it with four vCPUs, 3 GiB RAM, an NVMe disk and a virtual RTL8139 NIC.
Firefox uses a fresh temporary profile with its content sandbox disabled for
this compatibility fixture. A host-local HTTP page allocates and rotates
192 MiB of JavaScript arrays, paints a canvas, and reports progress. A separate
native process measures 10 ms sleeps and records memory, TLB and scheduler
snapshots. These are headless software-rendered tests; neither the compositor
nor hardware-accelerated video/WebGL is covered.

Early attempts using a guest-local HTTP listener were invalid: the IPv4 send
path does not locally deliver loopback traffic. These attempts are retained
as diagnostic logs, not successful workload runs. The host-local server fixes
the fixture without changing networking code.

A first repaired 120-second run started its page too late and completed only
63 workload cycles, below the fixture's 100-cycle minimum. It is not counted
as a passing browser test. Closing the browser released most of its memory;
the following 20 seconds had a maximum wake delay of 15 ms and empty CPU
runqueues apart from the monitor. A lasting post-close slowdown was not
reproduced in that run. A subsequent HTTP/1.0 run also stopped reporting
progress and failed the completion check. The fixture now serializes progress
requests over persistent HTTP/1.1 connections rather than accumulating requests
and short-lived connections; this is a test change, not a claimed network fix.

The final repaired run passed: 329 reported allocation/canvas cycles and the
page's explicit 60-second completion notification. Firefox remained open for
three minutes, then its process group was terminated and monitored for another
20 seconds. The longest 10 ms sleep took 200 ms during startup, at most 32 ms
in later sampling windows, and 14 ms after termination. No kernel fault or PMM
corruption marker appeared. The comparison kernel also completed a separate
three-minute run with 945 reported cycles and an 11 ms post-close maximum;
that earlier fixture ran the workload continuously. These different workload
durations are not an end-to-end browser performance comparison. Neither run
establishes that the user's lasting desktop slowdown is fixed.

The verified replacement kernel is installed in `avoryos-x86_64.iso`, retaining
the production Limine configuration. The extracted ISO kernel was compared
against the intended stripped ELF before atomic replacement. Rollback ISO:
`build/firefox-slowdown/iso-before.iso`. Its installed replacement hash is in
`build/firefox-slowdown/installed-iso.sha256`. The desktop disk was not modified.

Reproduction commands:

```sh
python3 scripts/tests/test_vma_merge_lifetime.py
python3 scripts/tests/test_vma_merge_latency_boot.py
AVORY_TEST_CPUS=1 python3 scripts/tests/test_vma_merge_latency_boot.py
AVORY_TEST_CPUS=8 python3 scripts/tests/test_vma_merge_latency_boot.py
python3 scripts/tests/test_firefox_responsiveness_boot.py
```

The VM commands require host KVM access. Evidence and comparison kernels are
under `build/firefox-slowdown/`. The source desktop `disk.img` is only read
while creating the private copy; QEMU attaches the copy.
