# Automatic compressed swap

AvoryOS now compresses eligible cold anonymous pages in RAM, restoring them on
page fault. It starts automatically before userspace. No disk swap file, disk
image rebuild, `mkswap`, or `swapon` command is needed.

The default logical capacity is half of physical RAM, capped at 2 GiB. The
storage budget is one quarter of physical RAM, including the slot table,
allocated packed frames and a conservative descriptor charge. LZ4 blocks must
fit in 2 KiB; incompressible pages stay resident. Repeated 64-bit patterns use
only slot metadata. Packed classes range from 128 bytes to 2 KiB, allowing
multiple compressed objects to share each physical frame. An empty packed
frame is returned to the physical allocator immediately.

Only private anonymous 4 KiB pages with a single physical mapping reference
are candidates. Shared memory, device/file mappings, shared CoW frames,
MAP_LOCKED mappings and actual huge pages are excluded. A second-chance walk
uses the accessed bit, rotates address spaces, and bounds VM work per pass.
The daemon runs under memory pressure. Faulting allocators also reclaim, and
wait off-lock for another active compressor rather than racing it to the last
frame. Unproductive scanning is bounded, so a full store or incompressible
workload can reach the normal allocation-failure path.

## Configuration and status

Kernel command-line settings:

- `zram=0`: disable compressed swap at boot.
- `zram_mb=N`: logical capacity in MiB, capped at 2048.
- `zram_limit_mb=N`: storage budget in MiB, capped at half of physical RAM.

A configuration whose metadata table cannot fit its budget is not activated.
The initialization message reports the selected capacity and limit.

Read status using:

```sh
cat /proc/swaps
cat /proc/zram
cat /sys/block/zram0/mm_stat
```

`/proc/meminfo` and `sysinfo()` report swap capacity/free space. `/proc/zram`
adds actual page-out/page-in counts, scan work, pressure yields, rejected
compression attempts and allocation failures. `mm_stat` reports original bytes,
compressed bytes, charged memory, memory limit, peak memory and same-pattern
page count, followed by zeroes for unsupported compaction/huge-object fields.
Read-only sysfs attributes also expose `disksize`, `mem_limit`, `initstate` and
`comp_algorithm`.

`/dev/zram0` provides a root-only, read-only diagnostic view of the active
store. It is already managed by the native swap implementation: manually
formatting it, activating it with `swapon`, resizing/resetting it, changing
algorithms, disk writeback and live `swapoff` are not implemented. This is a
native automatic swap implementation, not the complete Linux zram control ABI.
The [Linux zram documentation](https://docs.kernel.org/admin-guide/blockdev/zram.html)
describes that broader ABI.

`mlock` faults in the requested pages and marks them so compressed reclaim
skips them; `munlock` clears those marks. `mlockall(MCL_CURRENT)` applies the
same behavior to current mappings. `MCL_FUTURE` currently keeps the whole
address space out of compressed reclaim until `munlockall`, including mappings
that existed before the call. Lock state is not inherited across fork.
`mlock2(MLOCK_ONFAULT)` is still unsupported. Anonymous mappings created with
MAP_LOCKED are also excluded from compressed reclaim. Per-process
`RLIMIT_MEMLOCK` accounting is not implemented yet.
`MADV_COLD` is now a content-preserving hint, and `MADV_PAGEOUT` compresses
eligible pages instead of discarding them. Both had previously followed the
anonymous discard path.

## Ownership and races

A candidate frame is pinned and write-protected under the address-space and
page-table locks. Synchronous page-scoped TLB shootdowns remove stale writable
aliases before compression reads its data, then remove the old mapping before
its frame is freed. They avoid evicting translations from unrelated active
address spaces; cached PCID entries on inactive CPUs are invalidated before
that address space runs again. Compression and allocation happen off those
locks. Installation
revalidates the VMA, PTE, frame and exclusive mapping count; a concurrent write,
fork or replacement cancels installation safely. Read-only PTEs never acquire
a new CoW permission during freezing, including when compression fails.

A compressed slot is immutable. Each PTE and in-flight reader owns a reference.
Fork retains slot references; each fault restores a private physical copy.
Unmap, DONTNEED, fixed replacement, exec and exit release slot references.
Empty-table reclamation treats compressed PTEs as occupied. Fault-around cannot
replace a compressed PTE with a zero page. Protection changes are checked again
when installing a restored page, and mremap copies compressed source contents.

## Heavy tests

Run the complete suite:

```sh
python3 scripts/tests/run_zram_suite.py
```

It builds only the kernel, never the main ISO/disk prerequisites. Every boot
uses a separate minimal disk under `build/`; no VM opens the desktop `disk.img`.
The suite retains logs under `build/zram-suite/` and includes:

- 60,000 codec roundtrips cross-checked in both directions against liblz4,
  60,000 malformed-block fuzz cases, and output-bound checks under ASan/UBSan.
- Eight host threads performing 160,000 total object lifetimes with reader
  references and byte-for-byte checks. Heap/PMM failure injection, exhausted
  slots, quota exhaustion, reuse and full packed-frame reclamation are checked.
- Real page faults, fork isolation, child exit/exec, read-only and PROT_NONE
  faults, rejected-compression permissions, DONTNEED, partial unmap, MAP_FIXED,
  forced mremap, shared exclusions and incompressible data retention.
- Eight guest threads repeatedly compressing/restoring/protecting mappings,
  plus 400 replacements racing a concurrent pageout thread.
- Eight allocating threads filling a working set 64 MiB larger than guest
  physical RAM, with no explicit pageout in that phase. Every word is verified
  in permuted and reverse order, with modifications surviving further swaps.
- Single-, four- and eight-vCPU configurations, disabled-mode validation and a
  4 MiB storage-limit test that verifies all bytes after pageouts are rejected.
- Existing fork/CoW and deferred address-space teardown regressions, extended
  to cover compressed-slot inheritance and allocation-failure unwind.

Host tests require a C compiler, pthreads, sanitizer support and liblz4 headers.
VM tests use the existing musl toolchain, QEMU, Limine/OVMF and KVM by default;
`AVORY_TEST_ACCEL=tcg` selects emulation. Detailed failures remain in serial logs.


## Recorded validation

The final implementation passed the host sanitizers, fork/CoW regression,
deferred teardown regression, and private KVM tests at 1, 4 and 8 vCPUs. Every
full VM case used 512 MiB RAM, eight allocating threads and a 576 MiB working
set. Disabled mode and the 4 MiB storage-limit case also passed. A real
simultaneous-allocation OOM exposed during development was corrected with
fault-path pressure backoff; the same workload subsequently passed.

| vCPUs | Automatic page-outs | Page-ins | Compressed objects left during pressure |
| --- | ---: | ---: | ---: |
| 1 | 247692 | 195930 | 51762 |
| 4 | 243434 | 191996 | 51438 |
| 8 | 245464 | 194041 | 51423 |

These counts cover only the oversubscription phase. The data pattern is
intentionally compressible and unique per page, so the observed ratio is not a
prediction for Minecraft or other applications. Full logs are retained under
`build/zram-suite/`. The rebuilt normal ISO and its kernel hashes are recorded
in `build/zram-package/manifest.json`.
