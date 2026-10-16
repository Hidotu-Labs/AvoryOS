# AvoryOS NVMe driver

The NVMe driver lives in `kernel/src/drivers/storage/nvme.c` (with register and
API definitions in `nvme.h`).  It drives one or more PCI NVMe controllers
through the device manager, exposes every active namespace as a 512-byte-sector
block device (`nvme0n1`, `nvme0n11`, …), and can serve as the root disk.

The interactive QEMU targets now serve the root filesystem from NVMe:
`make run` (KVM) and `make run-tcg` (TCG; `make run-x86_64` is the
arch-specific implementation behind it).  `make run-sata` and
`make run-sata-tcg` keep the legacy IDE/AHCI root disk for regression and A/B
comparison, while `make run-nvme` / `make run-nvme-tcg` are aliases of the
primary targets.

This document is the reference for the driver architecture, the build/runtime
knobs, the test tooling, and the Phase 7 hardening audit.

## Architecture

### Discovery and controller bring-up

1. `nvme_init()` registers a `DRIVER_KERNEL` device-manager driver matching PCI
   class `0x01`/subclass `0x08`; `nvme_probe()` runs for every function.
2. BAR0 is decoded as a 64-bit MMIO BAR (`nvme_bar0_phys`), mapped uncached at
   the HHDM slot (`nvme_map_mmio`), and the DM resource `bar0` supplies its
   size (16 KiB minimum).
3. CAP is validated: the NVM command set must be advertised (`CAP.CSS` bit 0)
   and `CAP.MPSMIN` must be 0 (4 KiB host pages).  Depth is
   `min(CAP.MQES + 1, 64)` and never below 2.
4. The controller is quiesced (`CC.EN=0`, wait for `CSTS.RDY=0`), PCI memory
   space and bus mastering are enabled, and completion delivery is requested
   before the first enable so the I/O CQs can be created with `IEN=1`.
5. `nvme_controller_enable_admin` writes `AQA/ASQ/ACQ`, then `CC` with
   `EN=1`, `CSS=NVM`, `MPS=4K`, `IOSQES=6`, `IOCQES=4`, and waits for
   `CSTS.RDY=1` (or `CSTS.CFS`, which aborts bring-up).
6. Namespaces are discovered with one Identify Active Namespace ID list
   (CNS 02h) command followed by one Identify Namespace per active ID.
   Controllers that reject CNS 02h fall back to probing every NSID up to
   `CAP.NN` (capped at `NVME_MAX_NAMESPACES`), so both paths register the same
   block devices without paying the per-NSID round-trips on modern drives.

### Queues

| Queue | QID | Depth | Notes |
| ----- | --- | ----- | ----- |
| Admin SQ/CQ | 0 | 64 or `MQES+1` | Polled, serialized by `admin_lock` |
| I/O SQ/CQ | 1..N | same | One pair per CPU up to `NVME_MAX_QUEUES=16` and the device's `max_ioqpairs` |

- Admin commands use a rotating CID (`admin_cid`) and spin on the CQ phase bit
  with a 5 s deadline; `CSTS.CFS` or a timeout fail-stops the controller.
- I/O queues are per-CPU: `nvme_current_vq()` prefers the current CPU, or a
  single-CPU affinity mask when the caller is pinned.  CPUs beyond the created
  queue count share modulo.
- Each `struct nvme_iovq` owns its lock, CID bitmap/verdicts/wait queues, and
  PRP list pages, so per-CPU submission never contends on a global lock.
- `nvme_create_io_queues` creates CQ first, then SQ bound to it, keeping any
  pairs that worked when `max_ioqpairs` is smaller than `wanted_queues`; if
  zero pairs can be created the controller is disabled and reported failed.

### Completion handling

- `nvme_drain_cq()` is the single CQ consumer (under `cq_lock`).  It consumes
  entries whose phase bit matches, records a once-only verdict per CID under
  the queue lock, wakes the CID's wait queue, and rings the CQ doorbell.
- `nvme_io_wait()` first takes a verdict, then drains, then checks
  `CSTS.CFS` and the 5 s deadline.  If the caller may block it sleeps on the
  per-CID wait queue with a `wakeup_ticks` fallback, so a lost interrupt
  degrades to polling instead of hanging.
- IRQ delivery is requested in the order MSI-X → MSI → INTx, else the driver
  masks interrupts and polls (`NVME_NOIRQ`, `NVME_DISABLE_MSIX`,
  `NVME_DISABLE_MSI` force a lower mode).  MSI-X allocates one vector per
  queue when the table allows it; the last vector drains the remaining queues.
- `NVME_FAULT_DROP_IRQ` consumes completions in the handler but skips the
  wakeup, exercising the scheduler-tick fallback path.

### Transfers

- Buffers are described by `PRP1` plus `PRP2` or chained PRP list pages.  One
  page of 512 entries, then chained pages (up to three) allow 4 MiB per
  command, including a page-misaligned first page.
- Transfers are capped by `min(NVME_MAX_TRANSFER, MDTS)` and chunked at
  namespace logical-block boundaries (`nvme_io_range`).
- User pointers and unaligned kernel buffers are staged through a physically
  contiguous bounce allocation (`nvme_io_bounce`); kernel buffers that are
  512-byte aligned and page-addressable are mapped directly.
- FUA is issued on `block_device.write_sectors_fua` and, with `NVME_FUA=1`, on
  every write.  Flush is `NVME_CMD_FLUSH` and participates in recovery.
- Namespaces with 1024/2048/4096-byte logical blocks are exposed as 512-byte
  virtual sectors.  Requests that only partially cover a logical block go
  through a per-namespace read-modify-write page serialized by
  `nvme_rmw_lock()`; fully aligned requests are translated 1:1.
- With `NVME_BATCH_IO=1`, namespaces additionally advertise
  `read_batch`/`write_batch`.  `nvme_batch_submit()` gathers a wave of up to
  `NVME_BATCH_WAVE_MAX` (16) independent fast members (aligned kernel buffer,
  whole logical blocks, within MDTS) and submits their commands back to back
  before any wait; `nvme_io_wait_many()` reaps every wave CID against one
  shared deadline and parks on a dedicated per-queue `batch_wq`, woken once
  per completion drain.  Members that need the full path (user buffers,
  bounce staging, partial-LBA RMW) fall back to `nvme_io()` one at a time
  while no batched command is outstanding.  `nvme_io_range()` chunks are
  pipelined the same way.  A batch call always waits for every submitted
  command before returning; completions across members may be observed out
  of order, so members must be independent and disjoint.

### Shutdown, recovery and fail-stop

- `nvme_shutdown()` writes `CC.SHN=normal`, waits up to 2 s for
  `CSTS.SHST=complete`, then clears `CC.EN`.  It runs from the reboot/power-off
  syscall path and logs `NVME-SHUTDOWN: <ctrl> SHST=complete|timeout`.
- A transport failure fail-stops the controller: `nvme_fail_stop()` marks it
  failed, disables `CC.EN` and waits for `CSTS.RDY=0`.  I/O returns `-2`.
- With `NVME_RESET_RECOVERY=1`, the failing path resets the controller once
  (bumping `generation`, which makes old waiters abandon their CID) and retries
  the command; concurrent recoveries are serialized through `recovering`.
- Per-controller state is independent: one controller's fail-stop does not
  disturb another controller's queues (checked by the self-test when
  `NVME_SELFTEST_FAULT_INDEX` targets one controller).

## Block device surface

- Names: `nvme0n1`, `nvme0n11`, … (controller + namespace).  Partitioning is
  handled by the generic block layer; `nvme0n1p1` style names appear once
  partitions exist.
- `sector_size = 512`, `total_sectors = NSZE * (LBA size / 512)`.
- `driver_data` points at the namespace; all four block operation slots
  (`read_sectors`, `write_sectors`, `write_sectors_fua`, `flush`) are filled,
  and `NVME_BATCH_IO=1` adds `read_batch`/`write_batch`.
- Block reads/writes bounds-check `lba + count` against `total_sectors` before
  touching the device.

## Build knobs

| Knob | Effect |
| ---- | ------ |
| `NVME_SELFTEST=1` | Compile the boot self-tests (`nvme_test_admin_cycles`, PRP chain, SHN, audit) |
| `NVME_SELFTEST_CYCLES=N` | Disable→enable→Identify cycles (default 50) |
| `NVME_SELFTEST_IDENTIFY_OPS=N` | Extra Identify commands on the last live controller |
| `NVME_SELFTEST_DATA=1` | Destructive 4Kn/multi-NS/recovery tests (scratch media only) |
| `NVME_SELFTEST_FAULT_TIMEOUT=1` | Wedge one Flush, verify fail-stop (or recovery) |
| `NVME_SELFTEST_FAULT_INDEX=N` | Restrict the fault-injection test to one controller |
| `NVME_RESET_RECOVERY=1` | Reset + retry a command after a transport failure |
| `NVME_BATCH_IO=1` | Advertise `read_batch`/`write_batch`; one submitter can keep up to `NVME_BATCH_WAVE_MAX=16` commands in flight per wait |
| `NVME_FUA=1` | Every block write carries FUA |
| `NVME_NOIRQ=1` | Never request an interrupt; polled completions |
| `NVME_DISABLE_MSIX=1` / `NVME_DISABLE_MSI=1` | Skip the respective interrupt mode |
| `NVME_FAULT_DROP_IRQ=1` | Consume completions without waking the waiter |

QEMU-side knobs used by the harness: `mdts=N`, `max_ioqpairs=N`,
`msix_qsize=N`, `num_queues=N`, `ioeventfd=on`, and namespace properties
`logical_block_size=4096`, `detached=on`.

## Testing

Guest tools (built into `nvme_test.img` by `scripts/nvme/create-nvme-test.sh`):

- `/bin/nvme_test auto` — picks a scratch device and runs the transfer matrix
  (512 B … 4 MiB at LBA 0/mid/last/random), 4-/8-thread passes, an optional
  64-thread `--deep` pass, SHA-256 reports, an optional `--seconds` soak and
  the Phase 7 `--verified-gib=N` ledger mode.
- `/bin/nvme_test raw|hash|info` — manual raw-device write/verify/hash.
- `/bin/nvme_bench <dev>` — sequential/random read/write benchmarks at a
  chosen block size and queue depth; `--sweep` checks qd4/qd1 scaling and
  `--compare` runs the same workload on an AHCI device.

Host orchestrator (`scripts/nvme/nvme-stress.sh`) builds the requested kernel
configuration, boots QEMU headless, checks the guest markers, verifies the
backing images (LE64 pattern, SHA-256, 4Kn/512e pair equality) and runs
`e2fsck -fn`/`-fy` where applicable.  Useful flags: `--selftest`,
`--data-selftest`, `--recovery`, `--fua`, `--4kn`, `--multi-ns`, `--pair`,
`--big`, `--deep`, `--mdts-matrix`, `--irq-matrix`, `--drop-irq`,
`--fault-timeout`, `--controllers=N`, `--no-namespace`, `--crash-consistency=N`,
`--soak=SECONDS`, `--bench*`, and the Phase 7 additions `--accel=kvm|tcg`,
`--verified-gib=N`, `--guest-args=…`.

Phase 7 acceptance suites (`scripts/nvme/nvme-phase7.sh`):

```sh
make nvme-phase7-dry                     # print the 72-cell matrix + commands
make nvme-phase7-quick                   # reduced smoke run of every suite
make nvme-phase7                         # full acceptance run
./scripts/nvme/nvme-phase7.sh matrix --list   # inspect the matrix
./scripts/nvme/nvme-phase7.sh soak --hours=4  # worst-case 4 h soak
./scripts/nvme/nvme-phase7.sh tib --target-gib=1024 --gib-per-boot=64
./scripts/nvme/nvme-phase7.sh chaos --cuts=100
./scripts/nvme/nvme-phase7.sh boots --count=50
./scripts/nvme/nvme-phase7.sh ahci --count=5
```

Suites are resumable: completed matrix cells, the verified-byte ledger and the
last chaos cut are recorded under `build/nvme/phase7/state/`; logs are written
to `build/nvme/phase7/`.  `--no-resume` starts over.

The 72-cell matrix is the Cartesian product of:

| Dimension | Values |
| --------- | ------ |
| LBA format | `512e` (root from NVMe), `4kn` (AHCI root + 4Kn/512e pair + data self-test) |
| Topology | single NS, multi-NS (1 GiB + 8 GiB + detached), multi-controller + no-namespace controller |
| Firmware knobs | baseline, constrained (`mdts=3`, `max_ioqpairs=2`, `msix_qsize=1`) |
| Accelerator | KVM (skipped when `/dev/kvm` is unavailable), TCG |
| SMP | 1, 2, 4 |

Every cell runs `NVME_SELFTEST=1` with 3 disable/enable cycles and 10000
Identify commands, so the Phase 2 admin-command stress is part of the matrix.

Long-running acceptance commands from the Phase 7 checklist:

```sh
# 4 h worst-case soak: 1 vCPU TCG, 4Kn + 512e, QD 64 randwrite, lost IRQ
./scripts/nvme/nvme-phase7.sh soak --hours=4

# 1 TiB cumulative verified writes (host-side ledger, resumable)
./scripts/nvme/nvme-phase7.sh tib --target-gib=1024 --gib-per-boot=64

# 100 power-cut + fsck cycles (journal replay, then clean -fn)
./scripts/nvme/nvme-phase7.sh chaos --cuts=100

# 50 boot-loop configurations
./scripts/nvme/nvme-phase7.sh boots --count=50

# AHCI/ATA regression with host pattern/SHA verification
./scripts/nvme/nvme-phase7.sh ahci --count=5
```

## Phase 7 hardening audit

Each audit item from the checklist maps to code and a repeatable check.

| Audit item | Implementation | Check |
| ---------- | -------------- | ----- |
| `NSZE << LBA shift` overflow | `nvme_ns_geometry()` validates `nsze`, `lbads`, metadata, `NSZE * units`, and caps the virtual sector count at `UINT64_MAX / 512` so `lba * 512` cannot wrap | `NVME-AUDIT` geometry cases; `NVME-4KN`; `--mdts`/4Kn matrix cells |
| `LBA + count` overflow and past-end access | `nvme_io()` rejects `lba >= total_sectors` and `count > total_sectors - lba` before submission; `nvme_io_direct_once()` rejects `NLB` outside 1..65536 | `NVME-AUDIT` request-bounds cases; transfer matrix at LBA 0/mid/last |
| MDTS arithmetic | `nvme_max_transfer_for()` clamps the shift below 64 and caps the result at `NVME_MAX_TRANSFER` before any 32-bit cast | `NVME-AUDIT` MDTS cases; `--mdts-matrix` and constrained matrix cells |
| DMA lifetime on timeout | `dma_quiesced` is cleared when `CC.EN=1` is written and set only after `CSTS.RDY=0`; `nvme_fail_stop()` waits for that before returning; bounce pages are freed only on success or when quiesced; release keeps DMA pages if the controller refuses to stop | `--fault-timeout` fail-stop test; `--recovery` test; audit documentation in `nvme.c` |
| IRQ release on probe failure | `nvme_irq_map_clear()` removes vector-map entries before `pci_irq_release()` recycles the vector; `nvme_controller_release()` is called on the `nvme_controller_start()` failure path | Review + `NVME-IRQ` mode markers under `--irq-matrix`; release-order comments |
| CID exhaustion | `nvme_cid_claim_locked()` scans a bounded bitmap; `nvme_alloc_cid()` drains, and after 5 s logs a diagnostic and fail-stops instead of spinning forever; `nvme_take_verdict`/`nvme_free_cid` bound-check the CID | `NVME-AUDIT` CID claim/exhaustion/reuse case; `--deep` 64-thread pass |
| Barriers | Acquire fence after the CQ phase check before reading `cid`/`result`; release fence between SQE/tail stores and the SQ doorbell in both submit paths; queue state under per-queue locks | Comments + `--deep`/contention and soak runs; code review |
| Bounce zeroing | Bounce pages, the per-namespace RMW page and PRP list pages are `memset` before first use, so a short/partial transfer cannot leak stale allocator bytes | Code review; destructive 4Kn RMW self-test |

The non-destructive audit cases run at every `NVME_SELFTEST=1` boot and print
`NVME-AUDIT: PASS|FAIL|SKIP`; `scripts/nvme/nvme-stress.sh --selftest` requires the
PASS marker.  Destructive cases stay behind `NVME_SELFTEST_DATA=1` and only
touch scratch media.

The NVMe sources (`nvme.c`, `nvme.h`, `nvme_selftest.c`, `nvme_test.c`,
`nvme_bench.c`) compile warning-free under `-Wall -Wextra` for every knob
combination in the table above; the rest of the kernel has pre-existing
warnings that are unrelated to this driver.

## Known limits

- Accepted LBA formats: 512 B to 4096 B, no metadata (`MS=0`); other formats
  are skipped with a log line.
- `NVME_MAX_CONTROLLERS=8`, `NVME_MAX_NAMESPACES=32`,
  `NVME_MAX_QUEUES=16`, `NVME_CID_MAX=64`, 4 MiB per command, three PRP list
  pages per CID.
- The admin queue is polled by design; admin commands are serialized.
- A controller that never clears `CSTS.RDY` after `CC.EN=0` is abandoned with
  its DMA buffers retained (bounded leak) rather than freed while it may still
  be writing.
- Multi-path/I/O virtualization (SR-IOV, CM B, persistent memory regions) and
  Namespace Management are out of scope.

## File map

| Path | Contents |
| ---- | -------- |
| `kernel/src/drivers/storage/nvme.c` | driver, self-tests, audit |
| `kernel/src/drivers/storage/nvme.h` | registers, opcodes, API |
| `kernel/src/tests/nvme/nvme_selftest.c` | boot self-test entry and markers |
| `userland/nvme_test.c` | guest transfer matrix / soak / verified-writes / crash tool |
| `userland/nvme_bench.c` | guest queue-scaling benchmark |
| `scripts/nvme/create-nvme-test.sh` | builds the minimal test root image |
| `scripts/nvme/nvme-stress.sh` | single-config QEMU boot + host verification |
| `scripts/nvme/nvme-phase7.sh` | Phase 7 acceptance suites and matrix |

## Standalone runner cleanup (2026-10-11)

The unused `run-nvme-test` Make target was removed. The NVMe driver, normal
NVMe boot targets, and the stress/Phase 7 suites remain available. Their
shared test-image builders and regression sources are retained.
