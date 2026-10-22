# KSM and application stalls (October 18)

The user reports slow Plasma 6, Alacritty and vkcube, with a working mouse,
keyboard and clock. The USB silent-endpoint messages are not established as
related. The manually requested Lockdiag report is retained in
`build/ksm-freeze-investigation/latest-lockdiag.log`.

The report at 115467 ms has acknowledgements from all eight CPUs, seven idle
CPUs and vkcube running on CPU 7. The journal is idle; vmm, GPU poll/present and
shootdown locks are free. Plasma and Qt render workers are parked on futexes;
Alacritty and Xorg have event-loop waits. These are snapshots, not proof of a
particular lost wake or a kernel deadlock. Wait durations alone do not establish
that an idle worker should have been woken. The original duplicate-daemon
report is also preserved under
`build/logs/prime-runs/20261018T153252Z-9svhkb5s/serial.log`.

## Private mapping futex identity

`futex_get_key` previously treated every operation without FUTEX_PRIVATE_FLAG
as physical-address keyed, including futexes in MAP_PRIVATE memory. A wait can
register on frame A, KSM/CoW can replace the mapping with frame B, and a wake
then looks up B and misses the waiter on A. Distinct private mappings of a
shared zero/KSM frame can also acquire the same physical key erroneously.

Private VMAs now use (mm, virtual address) even when the caller omits PRIVATE.
Explicit PRIVATE calls keep their original fast path. Truly shared and SysV
mappings retain physical keys, with VMA classification and translation protected
by mm->lock. This corrects the demonstrated replacement case; implementing
inode/offset identities for movable shared file-cache pages is separate work.
Linux also distinguishes private mappings from the operation's flag:
https://github.com/torvalds/linux/blob/v6.6/kernel/futex/core.c

## Bounded background scanning

A timer watchdog previously created ksmd before KSM initialization, which then
created a second daemon. The timer no longer creates KSM threads; initialization
is idempotent and creates one daemon at background priority/nice 19.

Each mm scan is limited to 256 VMA/PTE work units, counting absent PTEs too.
A cursor in mm, guarded by mm->lock, continues later passes and wraps at the
end. Hash-chain comparison has a 16-candidate limit. This bounds IRQ-masked
work even on huge sparse mappings or colliding hashes. The prior page pins,
write-protection flush, mapping revalidation and delayed frame release remain.
The tradeoff is slower deduplication of large or highly colliding mappings.

## Launch stalls traced to the replacement pass (October 18, follow-up)

The user measured intermittent Firefox launches of up to 30 s against a
3 s baseline. Two mechanisms in the replacement pass explained the worst
cases; both are removed.

* `mm->lock` was held across the hashing and 4 KiB memcmp loop, with
  interrupts masked. The user page-fault path takes the same lock
  (`vmm_fault.c`), so every fault in the scanned address space - thousands
  during an application launch - spun behind ksmd for milliseconds per
  pass, every 1.5 seconds. Content hashing, chain probing and memcmp now
  run without mm->lock: the frames are frozen, flushed and pinned, so
  their contents are stable, and the hash table is scan-local
  (ksm_scan_active excludes concurrent scanners). The lock is held again
  only for O(1) PTE revalidation and replacement; a decision whose PTE
  CoW-broke while the lock was dropped is discarded, since the pin routes
  any concurrent write into a fresh copy. Unmatched pages enter the table
  in the lockless phase, so within-batch dedup is preserved regardless of
  later revalidation.
* Scanning ran every 1.5 seconds unconditionally. The daemon now has two
  cadences. Under 128 MiB of free memory it scans eagerly (1.5 s base,
  backing off to 60 s when a pass merges nothing). Above it, dedup still
  runs because duplicates keep accruing with or without pressure, but at
  background cadence (15 s base, backing off to 2 min), which rarely
  overlaps an application launch and keeps the periodic tax small
  without disabling dedup entirely. Background passes are counted in
  `passes_idle` and exposed as `/proc/sys/vm/ksm_passes_idle`; the first
  pass in either mode logs the transition. Full-context shootdowns per
  frozen mm remain synchronous for flush-completion correctness;
  do_shootdown skips the IPI entirely when no remote CPU has the address
  space resident, so their cost is limited to address spaces that are
  actually running elsewhere.

Explicit passes (`/sys/ksm/run`, `drop_caches=3`) always scan immediately,
so a manual request bypasses whichever cadence is active. Per-pass merge
lines still print on actual merges only.

## Verification and limits

* Actual-source key test reproduces the old frame-dependent unmatched wake,
  verifies stable keys after replacement, mm isolation, shared/SysV aliases,
  and alignment/address failures, with ASan/UBSan.
* A private four-vCPU QEMU test parks a futex waiter without PRIVATE, discards
  its private backing with MADV_DONTNEED, faults in the shared zero page, then
  successfully wakes it: discard=0, value=0, woke=1, waited=0. Evidence is in
  `build/futex-cow-boot/serial.log`.
* Actual-source tests verify sparse-map/VMA work budgets, cursor progress/wrap,
  one background daemon, KSM frame lifetime and CoW revalidation.
* The concurrent-writer KSM boot test passes with one daemon and zero/same-page
  merging; futex requeue, deadline-tree and wake-publication tests pass.
* The kernel builds. The full interactive Plasma/AMDGPU performance workload
  has not been rerun with the final futex-key fix. These checks establish real
  defects and their fixes, not a measured desktop FPS improvement or proof
  that they account for every stall. The running user VM was not terminated.
