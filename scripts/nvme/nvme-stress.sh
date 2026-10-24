#!/usr/bin/env bash
# nvme-stress.sh - headless NVMe test harness for AvoryOS.
#
# Boots a QEMU configuration with the NVMe test image, waits for the guest to
# run /bin/nvme_test auto (the guest NVMe test init), checks the
# NVME-TEST/NVME-SELFTEST markers in the serial log, then verifies the written
# devices on the host: byte pattern, guest-vs-host SHA-256, 4Kn/512e pair
# equality and, for the nvme-root and crash configs, e2fsck -fy/-fn of the
# root partition.  Exits non-zero on any failure.
#
# Usage:
#   ./scripts/nvme/nvme-stress.sh [--config=ide-scratch|nvme-root] [--selftest]
#                            [--cycles=N] [--identify-ops=N]
#                            [--data-selftest] [--allow-data-selftest]
#                            [--recovery] [--fua]
#                            [--4kn] [--multi-ns] [--pair] [--big] [--deep]
#                            [--devices=d1,d2,...]
#                            [--controllers=N] [--no-namespace]
#                            [--mdts=N] [--mdts-matrix]
#                            [--max-ioqpairs=N] [--msix-qsize=N]
#                            [--irq-mode=msix|msi|intx|poll] [--irq-matrix]
#                            [--drop-irq] [--fault-timeout] [--fault-index=N]
#                            [--with-usb-gpu] [--boots=N]
#                            [--crash-consistency=N] [--crash-mb=N]
#                            [--soak=SECONDS] [--no-fsck] [--pattern-check]
#                            [--timeout=SECONDS] [--smp=N] [--phase5]
#
# Configs:
#   ide-scratch  root filesystem on the IDE/AHCI test image, NVMe controller
#                attached to a blank scratch disk.
#   nvme-root    root filesystem on the NVMe test image.
#
# --selftest rebuilds the kernel with NVME_SELFTEST=1 and requires the
# NVME-SELFTEST: PASS marker.  --cycles / --identify-ops configure that
# self-test (defaults: 50 disable/enable/Identify cycles, no extra ops).
#
# --irq-mode forces a completion delivery mode (msix|msi|intx|poll) and checks
# that the kernel reported it.  --irq-matrix runs all four in sequence.
# --drop-irq builds the lost-interrupt injection kernel; --fault-timeout adds
# the forced-timeout/fail-stop self-test (implies --selftest); --fault-index
# wedges only that controller (multi-controller isolation).  --with-usb-gpu
# adds xHCI + virtio-gpu so NVMe MSI-X coexists with other MSI-X users.
# --boots=N repeats the whole boot N times for regression runs.
#
# Phase 5 flags:
#   --4kn            attach a 4Kn (logical_block_size=4096) namespace and an
#                    identical 512e twin, then run the pair comparison.
#   --multi-ns       attach 1 GiB and 8 GiB namespaces plus a detached one.
#   --pair           guest writes the same pattern to two devices and compares.
#   --big            extend the guest matrix to 8/16 MiB transfers (mdts stress).
#   --deep           64-thread pass over the queue (CID reuse/contention).
#   --devices=LIST   explicit comma-separated guest devices.
#   --data-selftest  enable the destructive kernel 4Kn/multi-NS/recovery tests;
#                    only allowed for scratch media (--allow-data-selftest
#                    overrides the nvme-root refusal).
#   --recovery       build NVME_RESET_RECOVERY=1 (timeout -> reset -> retry).
#   --fua            build NVME_FUA=1 (every write carries FUA).
#   --mdts-matrix    run mdts=3, 7 and 0 (clamped to 9) with 16 MiB transfers.
#   --crash-consistency=N
#                    run N power-cut boots: the guest verifies the file left by
#                    the previous cut, fsyncs a fresh one and reboots without
#                    syncing; the host runs e2fsck -fy then -fn after each cut.
#   --phase5         the whole Phase 5 combination: selftest + data-selftest +
#                    recovery + FUA build plus one multi-NS/4Kn/pair/big/deep
#                    boot (ide-scratch only).
#
# Phase 6 benchmark flags (guest runs /bin/nvme_bench):
#   --bench                run one measurement on the NVMe namespace
#   --bench-sweep          measure qd 1/2/4/8 and enforce the qd4/qd1 ratio
#   --bench-mode=M         seqread|seqwrite|randread|randwrite (default seqread)
#   --bench-block=N        request size (default 4096)
#   --bench-qd=N           worker threads = queue depth (default 1)
#   --bench-seconds=N      measurement window (default 5)
#   --bench-bytes=N        region to touch (default 256 MiB)
#   --bench-compare        also run the same workload on /dev/sata02 (AHCI)
#   --bench-cold           evict the bench backing image from the host page
#                          cache before boot (posix_fadvise, no root needed)
#   --bench-matrix         one cold boot per qd (1/2/4/8) and enforce the
#                          qd4/qd1 scaling ratio from the separate runs
#   --min-scale=X          required qd4/qd1 IOPS ratio (default 1.5)
#   --num-queues=N         QEMU nvme num_queues knob (default auto)
#   --ioeventfd            QEMU nvme ioeventfd=on (doorbells without vmexits)
#   --drive-cache=none     host-side O_DIRECT + native AIO for the NVMe drive
#
# Long-running Phase 6 stress examples:
#   ./scripts/nvme/nvme-stress.sh --bench --bench-mode=randwrite --bench-qd=8 \
#       --bench-seconds=3600            # 8-thread x QD64-ish contention soak
#   ./scripts/nvme/nvme-stress.sh --bench-sweep --bench-seconds=600  # scaling soak
#   ./scripts/nvme/nvme-stress.sh --bench-compare --bench-qd=4       # NVMe vs AHCI
#
# Phase 7 acceptance flags (driven by scripts/nvme/nvme-phase7.sh):
#   --accel=kvm|tcg      force an accelerator instead of auto-detecting KVM
#                        (the matrix uses this for its KVM/TCG dimension).
#   --verified-gib=N     guest runs N GiB of verified write+read passes over
#                        the selected device and prints NVME-VERIFIED totals;
#                        the host checks the PASS marker (1 TiB ledger).
#   --guest-args=ARGS    extra arguments for the guest test command, after the
#                        flags this script generated (escape hatch for the
#                        Phase 7 runner; arguments are not shell-expanded).
#
# Ready-made Phase 7 entry points live in scripts/nvme/nvme-phase7.sh:
#   make nvme-phase7          # all acceptance suites
#   make nvme-phase7-quick    # reduced counts for a smoke run
#   make nvme-phase7-dry      # print the 72-cell matrix and commands
#
# Scaling acceptance (cold cache, host O_DIRECT + native AIO + ioeventfd; this
# is the configuration that takes QEMU's per-request CPU cost out of the way):
#   ./scripts/nvme/nvme-stress.sh --bench-matrix --bench-mode=randread \
#       --bench-seconds=5 --min-scale=1.5 --ioeventfd --drive-cache=none
#
# The transfer matrix (512 B .. 4 MiB at LBA 0/mid/last/random, 4- and
# 8-thread passes, SHA-256 report) runs inside /bin/nvme_test auto's default
# path; --soak adds a repeated verified pass until the given number of seconds
# has elapsed.  --controllers attaches extra blank NVMe drives, --no-namespace
# attaches a controller without a namespace and the QEMU knobs set
# mdts/max_ioqpairs/msix_qsize on every NVMe controller.

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT_DIR"

ORIG_ARGS=("$@")

CONFIG="ide-scratch"
TIMEOUT=""
SELFTEST=0
DATA_SELFTEST=0
ALLOW_DATA_SELFTEST=0
RECOVERY=0
FUA=0
SMP=4
MEM="4G"
ISO="avoryos-x86_64.iso"
CYCLES=""
IDENTIFY_OPS=""
CONTROLLERS=1
NO_NAMESPACE=0
MDTS=""
MDTS_MATRIX=0
MAX_IOQPAIRS=""
MSIX_QSIZE=""
SOAK=""
FSCK=1
PATTERN_CHECK=0
IRQ_MODE=""
IRQ_MATRIX=0
DROP_IRQ=0
FAULT_TIMEOUT=0
FAULT_INDEX=""
WITH_USB_GPU=0
BOOTS=1
CRASHES=0
CRASH_MB=""
CRASH_RUN=0
CRASH_BOOT=0
BIG=0
DEEP=0
PAIR=0
FOURK=0
MULTI_NS=0
PHASE5=0
DEVICES=""
BENCH=0
BENCH_SWEEP=0
BENCH_MODE="seqread"
BENCH_BLOCK="4096"
BENCH_QD="1"
BENCH_SECONDS="5"
BENCH_BYTES=""
BENCH_COMPARE=0
BENCH_COLD=0
BENCH_MATRIX=0
BENCH_DEV=""
MIN_SCALE="1.5"
NUM_QUEUES=""
IOEVENTFD=0
DRIVE_CACHE="writeback"
ACCEL="auto"
VERIFIED_GIB=""
GUEST_ARGS=""

usage() {
    # Print the header comment block (line 2 up to the first empty line)
    # instead of a hard-coded line range so new flags cannot desynchronize it.
    sed -n '2,/^$/p' "$0" | sed 's/^# \{0,1\}//'
    exit 2
}

for arg in "$@"; do
    case "$arg" in
    --config=*) CONFIG="${arg#--config=}" ;;
    --timeout=*) TIMEOUT="${arg#--timeout=}" ;;
    --smp=*) SMP="${arg#--smp=}" ;;
    --selftest) SELFTEST=1 ;;
    --data-selftest) DATA_SELFTEST=1 ;;
    --allow-data-selftest) ALLOW_DATA_SELFTEST=1 ;;
    --recovery) RECOVERY=1 ;;
    --fua) FUA=1 ;;
    --cycles=*) CYCLES="${arg#--cycles=}" ;;
    --identify-ops=*) IDENTIFY_OPS="${arg#--identify-ops=}" ;;
    --controllers=*) CONTROLLERS="${arg#--controllers=}" ;;
    --no-namespace) NO_NAMESPACE=1 ;;
    --mdts=*) MDTS="${arg#--mdts=}" ;;
    --mdts-matrix) MDTS_MATRIX=1 ;;
    --max-ioqpairs=*) MAX_IOQPAIRS="${arg#--max-ioqpairs=}" ;;
    --msix-qsize=*) MSIX_QSIZE="${arg#--msix-qsize=}" ;;
    --irq-mode=*) IRQ_MODE="${arg#--irq-mode=}" ;;
    --irq-matrix) IRQ_MATRIX=1 ;;
    --drop-irq) DROP_IRQ=1 ;;
    --fault-timeout) FAULT_TIMEOUT=1 ;;
    --fault-index=*) FAULT_INDEX="${arg#--fault-index=}" ;;
    --with-usb-gpu) WITH_USB_GPU=1 ;;
    --boots=*) BOOTS="${arg#--boots=}" ;;
    --crash-consistency=*) CRASHES="${arg#--crash-consistency=}" ;;
    --crash-mb=*) CRASH_MB="${arg#--crash-mb=}" ;;
    --crash-run) CRASH_RUN=1 ;;
    --crash-boot=*) CRASH_BOOT="${arg#--crash-boot=}" ;;
    --soak=*) SOAK="${arg#--soak=}" ;;
    --no-fsck) FSCK=0 ;;
    --pattern-check) PATTERN_CHECK=1 ;;
    --devices=*) DEVICES="${arg#--devices=}" ;;
    --4kn) FOURK=1 ;;
    --multi-ns) MULTI_NS=1 ;;
    --pair) PAIR=1 ;;
    --big) BIG=1 ;;
    --deep) DEEP=1 ;;
    --bench) BENCH=1 ;;
    --bench-sweep)
        BENCH=1
        BENCH_SWEEP=1
        ;;
    --bench-mode=*)
        BENCH=1
        BENCH_MODE="${arg#--bench-mode=}"
        ;;
    --bench-block=*)
        BENCH=1
        BENCH_BLOCK="${arg#--bench-block=}"
        ;;
    --bench-qd=*)
        BENCH=1
        BENCH_QD="${arg#--bench-qd=}"
        ;;
    --bench-seconds=*)
        BENCH=1
        BENCH_SECONDS="${arg#--bench-seconds=}"
        ;;
    --bench-bytes=*)
        BENCH=1
        BENCH_BYTES="${arg#--bench-bytes=}"
        ;;
    --bench-compare)
        BENCH=1
        BENCH_COMPARE=1
        ;;
    --bench-cold) BENCH_COLD=1 ;;
    --bench-matrix) BENCH_MATRIX=1 ;;
    --min-scale=*) MIN_SCALE="${arg#--min-scale=}" ;;
    --num-queues=*) NUM_QUEUES="${arg#--num-queues=}" ;;
    --ioeventfd) IOEVENTFD=1 ;;
    --drive-cache=*) DRIVE_CACHE="${arg#--drive-cache=}" ;;
    --accel=*) ACCEL="${arg#--accel=}" ;;
    --verified-gib=*) VERIFIED_GIB="${arg#--verified-gib=}" ;;
    --guest-args=*) GUEST_ARGS="${arg#--guest-args=}" ;;
    --phase5)
        PHASE5=1
        SELFTEST=1
        DATA_SELFTEST=1
        RECOVERY=1
        FUA=1
        BIG=1
        DEEP=1
        MULTI_NS=1
        FOURK=1
        PAIR=1
        ;;
    -h | --help) usage ;;
    *)
        echo "nvme-stress.sh: unknown argument '$arg'" >&2
        exit 2
        ;;
    esac
done

case "$CONFIG" in
ide-scratch | nvme-root) ;;
*)
    echo "nvme-stress.sh: unknown config '$CONFIG'" >&2
    exit 2
    ;;
esac

case "$ACCEL" in
auto | kvm | tcg) ;;
*)
    echo "nvme-stress.sh: unknown --accel '$ACCEL' (kvm|tcg|auto)" >&2
    exit 2
    ;;
esac

if [ -n "$VERIFIED_GIB" ] && ! [[ "$VERIFIED_GIB" =~ ^[0-9]+$ ]]; then
    echo "nvme-stress.sh: --verified-gib needs a non-negative integer" >&2
    exit 2
fi

if [ "$BENCH_COMPARE" = 1 ] && [ "$CONFIG" != "ide-scratch" ]; then
    echo "nvme-stress.sh: --bench-compare needs the IDE/AHCI scratch disk" >&2
    echo "  (--config=ide-scratch)" >&2
    exit 2
fi

if [ "$DATA_SELFTEST" = 1 ]; then
    SELFTEST=1
    if [ "$CONFIG" != "ide-scratch" ] && [ "$ALLOW_DATA_SELFTEST" != 1 ]; then
        echo "nvme-stress.sh: --data-selftest writes to whole namespaces and is" >&2
        echo "  only safe when they are scratch media (--config=ide-scratch)." >&2
        echo "  Use --allow-data-selftest if you really mean it." >&2
        exit 2
    fi
fi

if [ "$FAULT_TIMEOUT" = 1 ]; then
    SELFTEST=1
fi

# --- Multi-boot / mode matrices -------------------------------------------
# These re-enter this script so each configuration gets its own build and boot.

LOG_DIR="build/nvme"
mkdir -p "$LOG_DIR"

run_children() {
    local filter1="$1"
    local filter2="$2"
    shift 2
    local child=()
    local a
    for a in "${ORIG_ARGS[@]}"; do
        case "$a" in
        $filter1 | $filter2) continue ;;
        esac
        child+=("$a")
    done
    "$0" "${child[@]}" "$@"
}

if [ "$BOOTS" -gt 1 ]; then
    echo "[*] Multi-boot regression: $BOOTS boots"
    for ((b = 1; b <= BOOTS; b++)); do
        echo "[*] --- boot $b/$BOOTS ---"
        run_children "--boots=*" "" || exit 1
    done
    echo "[*] RESULT: PASS ($BOOTS boots)"
    exit 0
fi

if [ "$IRQ_MATRIX" = 1 ]; then
    echo "[*] IRQ-mode matrix: msix -> msi -> intx -> poll"
    for mode in msix msi intx poll; do
        echo "[*] --- IRQ mode: $mode ---"
        run_children "--irq-matrix" "--irq-mode=*" --irq-mode="$mode" || exit 1
    done
    echo "[*] RESULT: PASS (IRQ matrix)"
    exit 0
fi

if [ "$MDTS_MATRIX" = 1 ]; then
    echo "[*] MDTS matrix: 3 -> 7 -> 0 (0 clamps to 9) x 16 MiB transfers"
    # The MDTS limit belongs to the NVMe controller: target its namespace, not
    # the IDE scratch partition the default pick would choose.
    if [ "$CONFIG" = "nvme-root" ]; then
        MDTS_DEV="/dev/nvme0n1p2"
    else
        MDTS_DEV="/dev/nvme0n1"
    fi
    for m in 3 7 0; do
        echo "[*] --- MDTS: $m ---"
        run_children "--mdts-matrix" "--mdts=*" --mdts="$m" --big --deep \
            --devices="$MDTS_DEV" || exit 1
    done
    echo "[*] RESULT: PASS (MDTS matrix)"
    exit 0
fi

if [ "$BENCH_MATRIX" = 1 ]; then
    # Each qd is measured in its own boot after the host evicts the backing
    # image from the page cache, so qd1 is genuinely latency-bound and the
    # ratio reflects queue concurrency rather than cache warmth.
    echo "[*] Cold-cache benchmark matrix: qd 1 -> 2 -> 4 -> 8 (min ratio $MIN_SCALE)"
    declare -A BENCH_IOPS=()
    for qd in 1 2 4 8; do
        echo "[*] --- qd=$qd ---"
        out="$LOG_DIR/bench-matrix-qd$qd.out"
        if ! run_children "--bench-matrix" "--bench-cold" --bench \
            --bench-cold --bench-qd="$qd" >"$out" 2>&1; then
            echo "[!] qd=$qd run failed; see $out" >&2
            tail -n 20 "$out" >&2
            echo "[*] RESULT: FAIL (bench matrix)"
            exit 1
        fi
        iops="$(sed -n "s/.* qd=$qd iops=\([0-9]*\).*/\1/p" "$out" | head -1)"
        if [ -z "${iops:-}" ]; then
            echo "[!] qd=$qd produced no iops line; see $out" >&2
            echo "[*] RESULT: FAIL (bench matrix)"
            exit 1
        fi
        BENCH_IOPS["$qd"]="$iops"
        echo "[*] qd=$qd iops=$iops"
    done
    ratio="$(awk -v a="${BENCH_IOPS[1]}" -v b="${BENCH_IOPS[4]}" \
        'BEGIN { if (a > 0) printf "%.2f", b / a; else print "0.00" }')"
    echo "[*] qd4/qd1 ratio=$ratio (min $MIN_SCALE)"
    if awk -v r="$ratio" -v m="$MIN_SCALE" 'BEGIN { exit !(r >= m) }'; then
        echo "[*] RESULT: PASS (bench matrix, cold cache, ratio=$ratio)"
        exit 0
    fi
    echo "[*] RESULT: FAIL (bench matrix, ratio=$ratio < $MIN_SCALE)"
    exit 1
fi

if [ "$CRASHES" -gt 0 ]; then
    echo "[*] Crash/fsync consistency: $CRASHES power cuts"
    for ((b = 1; b <= CRASHES; b++)); do
        echo "[*] --- crash $b/$CRASHES ---"
        run_children "--crash-consistency=*" "" --crash-run --crash-boot="$b" ||
            exit 1
    done
    echo "[*] RESULT: PASS (crash consistency x$CRASHES)"
    exit 0
fi

# Build-knob composition for the kernel rebuild.
MAKE_VARS=()
if [ "$SELFTEST" = 1 ]; then
    MAKE_VARS+=(NVME_SELFTEST=1)
    [ -n "$CYCLES" ] && MAKE_VARS+=(NVME_SELFTEST_CYCLES="$CYCLES")
    [ -n "$IDENTIFY_OPS" ] && MAKE_VARS+=(NVME_SELFTEST_IDENTIFY_OPS="$IDENTIFY_OPS")
fi
if [ "$DATA_SELFTEST" = 1 ]; then
    MAKE_VARS+=(NVME_SELFTEST_DATA=1)
fi
if [ "$RECOVERY" = 1 ]; then
    MAKE_VARS+=(NVME_RESET_RECOVERY=1)
fi
if [ "$FUA" = 1 ]; then
    MAKE_VARS+=(NVME_FUA=1)
fi
case "$IRQ_MODE" in
"") ;;
msix) ;;
msi) MAKE_VARS+=(NVME_DISABLE_MSIX=1) ;;
intx)
    MAKE_VARS+=(NVME_DISABLE_MSIX=1 NVME_DISABLE_MSI=1)
    ;;
poll) MAKE_VARS+=(NVME_NOIRQ=1) ;;
*)
    echo "nvme-stress.sh: unknown --irq-mode '$IRQ_MODE' (msix|msi|intx|poll)" >&2
    exit 2
    ;;
esac
if [ "$DROP_IRQ" = 1 ]; then
    MAKE_VARS+=(NVME_FAULT_DROP_IRQ=1)
fi
if [ "$FAULT_TIMEOUT" = 1 ]; then
    MAKE_VARS+=(NVME_SELFTEST=1 NVME_SELFTEST_FAULT_TIMEOUT=1)
    [ -n "$FAULT_INDEX" ] && MAKE_VARS+=(NVME_SELFTEST_FAULT_INDEX="$FAULT_INDEX")
    [ -z "$CYCLES" ] && CYCLES=1
fi

# QEMU 11 rejects mdts=0 ("unlimited") and any value where 2^mdts + 1 exceeds
# IOV_MAX.  Model the unlimited case with the largest legal value, 9 (2 MiB).
if [ -n "$MDTS" ]; then
    case "$MDTS" in
    0)
        echo "[*] Note: QEMU rejects mdts=0 (unlimited); using mdts=9 (2 MiB) instead" >&2
        MDTS=9
        ;;
    *)
        if [ "$MDTS" -gt 9 ] 2>/dev/null; then
            echo "[*] Note: QEMU rejects mdts=$MDTS; clamping to mdts=9 (2 MiB)" >&2
            MDTS=9
        fi
        ;;
    esac
fi

NVME_OPTS=""
[ -n "$MDTS" ] && NVME_OPTS="$NVME_OPTS,mdts=$MDTS"
[ -n "$MAX_IOQPAIRS" ] && NVME_OPTS="$NVME_OPTS,max_ioqpairs=$MAX_IOQPAIRS"
[ -n "$MSIX_QSIZE" ] && NVME_OPTS="$NVME_OPTS,msix_qsize=$MSIX_QSIZE"
[ -n "$NUM_QUEUES" ] && NVME_OPTS="$NVME_OPTS,num_queues=$NUM_QUEUES"
[ "$IOEVENTFD" = 1 ] && NVME_OPTS="$NVME_OPTS,ioeventfd=on"

DRIVE_OPTS=""
case "$DRIVE_CACHE" in
writeback) ;;
none) DRIVE_OPTS=",cache=none,aio=native" ;;
*)
    echo "nvme-stress.sh: unknown --drive-cache '$DRIVE_CACHE' (writeback|none)" >&2
    exit 2
    ;;
esac

mkdir -p "$LOG_DIR"
LOG="$LOG_DIR/$CONFIG-$(date +%Y%m%d-%H%M%S).log"
ROOT_COPY="$LOG_DIR/root-p1-$(basename "$LOG" .log).img"
MAP_FILE="$LOG_DIR/device-map"

# --- Namespace topology (phase 5) -----------------------------------------
NS_IMG=()
NS_MIB=()
NS_4K=()
NS_DET=()

add_ns() {
    NS_IMG+=("$1")
    NS_MIB+=("$2")
    NS_4K+=("$3")
    NS_DET+=("$4")
}

PAIR_A=""
PAIR_B=""
DETACHED_NSID=""

if [ "$FOURK" = 1 ] && [ "$MULTI_NS" = 0 ]; then
    add_ns "build/nvme/ns4k.img" 64 1 0
    add_ns "build/nvme/ns512.img" 64 0 0
elif [ "$MULTI_NS" = 1 ]; then
    add_ns "build/nvme/ns2.img" 1024 0 0
    add_ns "build/nvme/ns3.img" 8192 0 0
    if [ "$FOURK" = 1 ]; then
        add_ns "build/nvme/ns4k.img" 64 1 0
        add_ns "build/nvme/ns512.img" 64 0 0
    fi
    add_ns "build/nvme/nsdet.img" 64 0 1
fi

NS_NAMES=()
for i in "${!NS_IMG[@]}"; do
    NS_NAMES+=("/dev/nvme0n$((i + 2))")
done

for i in "${!NS_IMG[@]}"; do
    if [ "${NS_4K[$i]}" = 1 ]; then
        PAIR_A="${NS_NAMES[$i]}"
    elif [ -n "$PAIR_A" ] && [ -z "$PAIR_B" ] && [ "${NS_DET[$i]}" = 0 ] &&
        [ "${NS_MIB[$i]}" = 64 ]; then
        PAIR_B="${NS_NAMES[$i]}"
    fi
    if [ "${NS_DET[$i]}" = 1 ]; then
        DETACHED_NSID=$((i + 2))
    fi
done

GUEST_DEVLIST=""
if [ "$PAIR" = 1 ] && [ -n "$PAIR_A" ] && [ -n "$PAIR_B" ]; then
    GUEST_DEVLIST="$PAIR_A,$PAIR_B"
fi
for i in "${!NS_IMG[@]}"; do
    [ "${NS_DET[$i]}" = 1 ] && continue
    name="${NS_NAMES[$i]}"
    case ",$GUEST_DEVLIST," in
    *",$name,"*) ;;
    *) GUEST_DEVLIST="${GUEST_DEVLIST:+$GUEST_DEVLIST,}$name" ;;
    esac
done
if [ -n "$DEVICES" ]; then
    GUEST_DEVLIST="${GUEST_DEVLIST:+$GUEST_DEVLIST,}$DEVICES"
fi

if [ "$PAIR" = 1 ] && [ -z "$PAIR_A" ]; then
    case "$DEVICES" in
    *,*) ;;
    *)
        echo "nvme-stress.sh: --pair needs a 4Kn topology (--4kn) or" >&2
        echo "  --devices=a,b with two devices" >&2
        exit 2
        ;;
    esac
fi

echo "[*] NVMe stress: config=$CONFIG selftest=$SELFTEST data=$DATA_SELFTEST recovery=$RECOVERY fua=$FUA smp=$SMP controllers=$CONTROLLERS no_ns=$NO_NAMESPACE soak=${SOAK:-0}s irq=${IRQ_MODE:-auto} drop_irq=$DROP_IRQ fault_to=$FAULT_TIMEOUT accel=$ACCEL verified=${VERIFIED_GIB:-0}GiB qemu_opts='${NVME_OPTS#,}' devices='${GUEST_DEVLIST:-auto}'"
echo "[*] Serial log: $LOG"

# --- Build inputs ----------------------------------------------------------

if [ ! -f edk2-ovmf/ovmf-code-x86_64.fd ]; then
    echo "[*] Fetching OVMF..."
    make edk2-ovmf
fi

# Rebuild cleanly whenever the CPPFLAGS knobs change: make does not track
# CPPFLAGS, so a stale object would silently keep an older interrupt mode or a
# self-test/fault-injection build.
KNOB_SIG="selftest=$SELFTEST data=$DATA_SELFTEST recovery=$RECOVERY fua=$FUA cycles=${CYCLES:-} ops=${IDENTIFY_OPS:-} vars=${MAKE_VARS[*]:-}"
PREV_SIG=""
[ -f "$LOG_DIR/.kernel-knobs" ] && PREV_SIG="$(cat "$LOG_DIR/.kernel-knobs")"
if [ "$KNOB_SIG" != "$PREV_SIG" ]; then
    echo "[*] Kernel build knobs changed; cleaning..."
    make -C kernel clean >/dev/null 2>&1
fi

if [ ${#MAKE_VARS[@]} -gt 0 ] || [ "$SELFTEST" = 1 ]; then
    echo "[*] Rebuilding kernel with: ${MAKE_VARS[*]:-NVME_SELFTEST=1}"
    BUILD_LOG="$LOG_DIR/build-knobs.log"
    if ! make "$ISO" "${MAKE_VARS[@]}" >"$BUILD_LOG" 2>&1; then
        echo "[!] kernel build failed; see $BUILD_LOG" >&2
        tail -n 40 "$BUILD_LOG" >&2 || true
        exit 1
    fi
else
    # Guard against a manual `make NVME_SELFTEST=1` left in the tree.
    if [ -f kernel/bin-x86_64/kernel ] &&
        grep -q "NVME-SELFTEST\|NVME-SELFTEST-DATA" kernel/bin-x86_64/kernel 2>/dev/null; then
        echo "[*] Kernel still contains the NVMe self-test; rebuilding clean..."
        make -C kernel clean >/dev/null 2>&1
    fi
    if [ ! -f "$ISO" ] || [ ! -x kernel/bin-x86_64/kernel ]; then
        echo "[*] Building $ISO..."
        BUILD_LOG="$LOG_DIR/build.log"
        make "$ISO" >"$BUILD_LOG" 2>&1 || {
            echo "[!] build failed; see $BUILD_LOG" >&2
            tail -n 40 "$BUILD_LOG" >&2 || true
            exit 1
        }
    else
        # Incremental: the kernel may be newer than the ISO after a fix.
        make "$ISO" >"$LOG_DIR/build.log" 2>&1 || {
            echo "[!] build failed; see $LOG_DIR/build.log" >&2
            tail -n 40 "$LOG_DIR/build.log" >&2 || true
            exit 1
        }
    fi
fi
printf '%s' "$KNOB_SIG" >"$LOG_DIR/.kernel-knobs"

# Guest command line.  The crash modes only verify/write/reboot; bench modes
# run /bin/nvme_bench; everything else composes the phase 3/5 matrix from the
# topology.
TEST_CMD=""
if [ "$CRASH_RUN" = 1 ]; then
    NVME_TEST_ARGS="--crash --fast"
    [ -n "$CRASH_MB" ] && NVME_TEST_ARGS="$NVME_TEST_ARGS --crash-mb=$CRASH_MB"
elif [ "$BENCH" = 1 ]; then
    BENCH_DEV=""
    if [ "$CONFIG" = "nvme-root" ]; then
        BENCH_DEV="/dev/nvme0n1p2"
    else
        BENCH_DEV="/dev/nvme0n1"
    fi
    if [ -n "$DEVICES" ]; then
        BENCH_DEV="${DEVICES%%,*}"
    fi
    TEST_CMD="/bin/nvme_bench $BENCH_DEV"
    NVME_TEST_ARGS="--mode=$BENCH_MODE --block=$BENCH_BLOCK --seconds=$BENCH_SECONDS"
    if [ "$BENCH_SWEEP" = 1 ]; then
        NVME_TEST_ARGS="$NVME_TEST_ARGS --sweep --min-scale=$MIN_SCALE"
    else
        NVME_TEST_ARGS="$NVME_TEST_ARGS --qd=$BENCH_QD"
    fi
    [ -n "$BENCH_BYTES" ] && NVME_TEST_ARGS="$NVME_TEST_ARGS --bytes=$BENCH_BYTES"
    [ "$BENCH_COMPARE" = 1 ] && NVME_TEST_ARGS="$NVME_TEST_ARGS --compare=/dev/sata02"
else
    NVME_TEST_ARGS=""
    [ -n "$GUEST_DEVLIST" ] && NVME_TEST_ARGS="--devices=$GUEST_DEVLIST"
    [ "$PAIR" = 1 ] && NVME_TEST_ARGS="$NVME_TEST_ARGS --pair"
    [ "$BIG" = 1 ] && NVME_TEST_ARGS="$NVME_TEST_ARGS --big"
    [ "$DEEP" = 1 ] && NVME_TEST_ARGS="$NVME_TEST_ARGS --deep"
    [ -n "$SOAK" ] && NVME_TEST_ARGS="$NVME_TEST_ARGS --seconds=$SOAK"
    NVME_TEST_ARGS="${NVME_TEST_ARGS# }"
fi

# Phase 7 extras only apply to the /bin/nvme_test auto path.
if [ "$CRASH_RUN" = 0 ] && [ "$BENCH" = 0 ]; then
    [ -n "$VERIFIED_GIB" ] &&
        NVME_TEST_ARGS="$NVME_TEST_ARGS --verified-gib=$VERIFIED_GIB"
    [ -n "$GUEST_ARGS" ] && NVME_TEST_ARGS="$NVME_TEST_ARGS $GUEST_ARGS"
    NVME_TEST_ARGS="${NVME_TEST_ARGS# }"
fi

# Force a rebuild when the baked test-init command line changes (soak options,
# topology device lists, crash/bench mode), even though make only tracks files.
ARGS_STAMP="$LOG_DIR/.nvme-test-args"
WANT_ARGS="$TEST_CMD|$NVME_TEST_ARGS"
if [ ! -f "$ARGS_STAMP" ] || [ "$(cat "$ARGS_STAMP")" != "$WANT_ARGS" ]; then
    rm -f nvme_test.img
fi
printf '%s' "$WANT_ARGS" >"$ARGS_STAMP"
echo "[*] Building nvme_test.img (guest: ${TEST_CMD:-/bin/nvme_test auto} $NVME_TEST_ARGS)..."
TEST_CMD="$TEST_CMD" NVME_TEST_ARGS="$NVME_TEST_ARGS" make nvme_test.img

# shellcheck disable=SC1091
[ -f build/nvme/nvme_test.layout ] && . build/nvme/nvme_test.layout
TEST_IMG="${TEST_IMG:-nvme_test.img}"
SCRATCH_IMG="${SCRATCH_IMG:-nvme_scratch.img}"
P1_START="${P1_START:-2048}"
P2_START="${P2_START:-0}"

# Clear the start of the scratch partition so a stale pattern from an earlier
# boot can never satisfy the host pattern/hash checks.
if [ -f build/nvme/nvme_test.layout ] && [ "$(basename "$TEST_IMG")" = "nvme_test.img" ]; then
    dd if=/dev/zero of="$TEST_IMG" bs=512 seek="$P2_START" count=8192 \
        conv=notrunc status=none
fi

# --- Host device map -------------------------------------------------------
# Guest name -> host image and byte offset, consumed by the Python verifier.

: >"$MAP_FILE"
map_add() { printf '%s %s %s\n' "$1" "$2" "$3" >>"$MAP_FILE"; }

if [ "$CONFIG" = "ide-scratch" ]; then
    map_add nvme0n1 "$SCRATCH_IMG" 0
    map_add sata0 "$TEST_IMG" 0
    map_add sata01 "$TEST_IMG" $((P1_START * 512))
    map_add sata02 "$TEST_IMG" $((P2_START * 512))
    map_add sda "$TEST_IMG" 0
    map_add sda1 "$TEST_IMG" $((P1_START * 512))
    map_add sda2 "$TEST_IMG" $((P2_START * 512))
else
    map_add nvme0n1 "$TEST_IMG" 0
    map_add nvme0n11 "$TEST_IMG" $((P1_START * 512))
    map_add nvme0n12 "$TEST_IMG" $((P2_START * 512))
    map_add nvme0n1p1 "$TEST_IMG" $((P1_START * 512))
    map_add nvme0n1p2 "$TEST_IMG" $((P2_START * 512))
fi
for i in "${!NS_IMG[@]}"; do
    map_add "${NS_NAMES[$i]#/dev/}" "${NS_IMG[$i]}" 0
done

# Cold-cache benchmarks: evict the bench device's backing image from the host
# page cache so qd1 sees storage latency instead of cached reads
# (posix_fadvise needs no root).
if [ "$BENCH" = 1 ] && [ "$BENCH_COLD" = 1 ] && [ -n "$BENCH_DEV" ]; then
    python3 - "$MAP_FILE" "$BENCH_DEV" <<'PY'
import os
import sys

map_path, dev = sys.argv[1], sys.argv[2].rsplit("/", 1)[-1]
images = []
with open(map_path) as f:
    for line in f:
        parts = line.split()
        if len(parts) == 3 and parts[0] == dev:
            images.append(parts[1])
for img in images:
    try:
        fd = os.open(img, os.O_RDONLY)
        os.posix_fadvise(fd, 0, 0, os.POSIX_FADV_DONTNEED)
        os.close(fd)
        print(f"cold: evicted {img} from host page cache")
    except OSError as exc:
        print(f"cold: fadvise {img} failed: {exc}")
PY
fi

# The legacy byte-pattern check only makes sense when the guest tested the
# purpose-built scratch partition (no explicit device list).
PATTERN_IMAGE=""
PATTERN_BASE=""
if [ "$CRASH_RUN" = 0 ] && [ "$BENCH" = 0 ] &&
    { [ -z "$GUEST_DEVLIST" ] || [ "$PATTERN_CHECK" = 1 ]; }; then
    if [ "$CONFIG" = "nvme-root" ]; then
        PATTERN_IMAGE="$TEST_IMG"
        PATTERN_BASE=$((P2_START * 512))
    else
        PATTERN_IMAGE="$TEST_IMG"
        PATTERN_BASE=$((P2_START * 512))
    fi
fi

# --- QEMU ------------------------------------------------------------------

KVM_ARGS=()
case "$ACCEL" in
auto)
    if [ -r /dev/kvm ] && [ -w /dev/kvm ]; then
        KVM_ARGS=(-cpu host -enable-kvm)
        echo "[*] Using KVM"
    else
        echo "[*] /dev/kvm unavailable, using TCG"
    fi
    ;;
kvm)
    if [ ! -r /dev/kvm ] || [ ! -w /dev/kvm ]; then
        echo "nvme-stress.sh: --accel=kvm requires /dev/kvm" >&2
        exit 2
    fi
    KVM_ARGS=(-cpu host -enable-kvm)
    echo "[*] Using KVM (forced)"
    ;;
tcg)
    echo "[*] Using TCG (forced, no KVM)"
    ;;
esac

COMMON_ARGS=(
    -M q35
    -m "$MEM"
    -drive "if=pflash,unit=0,format=raw,file=edk2-ovmf/ovmf-code-x86_64.fd,readonly=on"
    -cdrom "$ISO"
)

DISK_ARGS=()
case "$CONFIG" in
ide-scratch)
    DISK_ARGS=(
        -drive "file=$TEST_IMG,format=raw,if=none,id=root0"
        -device "ide-hd,drive=root0,bus=ide.0"
        -drive "file=$SCRATCH_IMG,format=raw,if=none,id=nvmed0$DRIVE_OPTS"
        -device "nvme,id=nvme0,serial=avoryos-test,drive=nvmed0$NVME_OPTS"
    )
    if [ ! -f "$SCRATCH_IMG" ]; then
        echo "[*] Building $SCRATCH_IMG..."
        ./scripts/nvme/create-nvme-test.sh >/dev/null
    fi
    ;;
nvme-root)
    DISK_ARGS=(
        -drive "file=$TEST_IMG,format=raw,if=none,id=nvmed0$DRIVE_OPTS"
        -device "nvme,id=nvme0,serial=avoryos-test,drive=nvmed0$NVME_OPTS"
    )
    ;;
esac

# Extra namespaces on the primary controller (phase 5 topology).  The pair
# images are zeroed so the host-side byte comparison is exact.
for i in "${!NS_IMG[@]}"; do
    img="${NS_IMG[$i]}"
    mib="${NS_MIB[$i]}"
    if [ ! -f "$img" ] || [ "$(stat -c%s "$img")" != "$((mib * 1024 * 1024))" ]; then
        truncate -s "${mib}M" "$img"
    fi
    if [ "$PAIR" = 1 ]; then
        case "$img" in
        build/nvme/ns4k.img | build/nvme/ns512.img)
            dd if=/dev/zero of="$img" bs=1M count="$mib" conv=notrunc \
                status=none
            ;;
        esac
    fi
    ns_prop=""
    if [ "${NS_4K[$i]}" = 1 ]; then
        ns_prop=",logical_block_size=4096,physical_block_size=4096"
    fi
    if [ "${NS_DET[$i]}" = 1 ]; then
        ns_prop="$ns_prop,detached=on"
    fi
    DISK_ARGS+=(
        -drive "file=$img,format=raw,if=none,id=nsext$i"
        -device "nvme-ns,drive=nsext$i,bus=nvme0$ns_prop"
    )
done

# Extra blank controllers for multi-controller / queue-knob coverage.
for ((i = 2; i <= CONTROLLERS; i++)); do
    EXTRA="build/nvme/extra$i.img"
    if [ ! -f "$EXTRA" ]; then
        truncate -s 64M "$EXTRA"
    fi
    DISK_ARGS+=(
        -drive "file=$EXTRA,format=raw,if=none,id=nvme$i"
        -device "nvme,id=nvme$i,serial=avoryos-extra$i,drive=nvme$i$NVME_OPTS"
    )
done

# A controller with no namespace is a valid NVMe configuration (capacity
# management environments use it, and the driver must not assume NS 1 exists).
if [ "$NO_NAMESPACE" = 1 ]; then
    DISK_ARGS+=(-device "nvme,serial=avoryos-nons$NVME_OPTS")
fi

# Optional xHCI + virtio-gpu so NVMe MSI-X shares the vector space with the
# other MSI-X users in the normal desktop configuration.
if [ "$WITH_USB_GPU" = 1 ]; then
    DISK_ARGS+=(
        -device "qemu-xhci,id=xhci"
        -device "usb-kbd,bus=xhci.0"
        -device "virtio-vga"
    )
fi

if [ -z "$TIMEOUT" ]; then
    TIMEOUT=300
    if [ -n "$SOAK" ]; then
        TIMEOUT=$((SOAK + 300))
    fi
    if [ -n "$GUEST_DEVLIST" ]; then
        TIMEOUT=$((TIMEOUT + 300))
    fi
    if [ -n "$VERIFIED_GIB" ]; then
        # Verified write+read passes dominate the boot; 60 s/GiB keeps a slow
        # TCG run from being killed mid-ledger.
        TIMEOUT=$((TIMEOUT + VERIFIED_GIB * 60))
    fi
    if [ "$CRASH_RUN" = 1 ]; then
        TIMEOUT=240
    fi
    if [ "$BENCH" = 1 ]; then
        if [ "$BENCH_SWEEP" = 1 ]; then
            TIMEOUT=$((BENCH_SECONDS * 4 + 300))
        else
            TIMEOUT=$((BENCH_SECONDS + 300))
        fi
        [ "$BENCH_COMPARE" = 1 ] && TIMEOUT=$((TIMEOUT + BENCH_SECONDS))
        [ -n "$BENCH_BYTES" ] && TIMEOUT=$((TIMEOUT + 300))
    fi
fi

echo "[*] Booting..."
qemu-system-x86_64 \
    "${COMMON_ARGS[@]}" \
    "${DISK_ARGS[@]}" \
    "${KVM_ARGS[@]}" \
    -smp "$SMP" \
    -display none \
    -serial "file:$LOG" \
    -no-reboot &
QEMU_PID=$!

START=$(date +%s)
while kill -0 "$QEMU_PID" 2>/dev/null; do
    NOW=$(date +%s)
    if [ $((NOW - START)) -ge "$TIMEOUT" ]; then
        echo "[!] Timeout after ${TIMEOUT}s; killing QEMU" >&2
        kill "$QEMU_PID" 2>/dev/null || true
        wait "$QEMU_PID" 2>/dev/null || true
        QEMU_PID=""
        break
    fi
    sleep 1
done
if [ -n "${QEMU_PID:-}" ]; then
    wait "$QEMU_PID" 2>/dev/null || true
fi

# --- Result checks ---------------------------------------------------------

fail() {
    echo "[!] $1" >&2
    if [ -f "$LOG" ]; then
        echo "[!] Last 40 serial lines:" >&2
        tail -n 40 "$LOG" >&2 || true
    fi
    exit 1
}

grep -q "NVME-TEST: PASS" "$LOG" || fail "guest raw test did not report PASS"

if grep -q "NVME-TEST: FAIL" "$LOG"; then
    fail "guest raw test reported FAIL"
fi

if grep -Eq "\[PANIC\]|KERNEL-MODE FAULT|HANG REPORT|\[ FATAL \]" "$LOG"; then
    fail "kernel reported a fatal failure during the run"
fi

if grep -q "NVME-MATRIX: FAIL\|NVME-THREADS: FAIL\|NVME-DEEP: FAIL\|NVME-PAIR: FAIL\|NVME-SOAK: FAIL\|NVME-CRASH-WRITE: FAIL\|NVME-CRASH-VERIFY: FAIL" "$LOG"; then
    fail "guest Phase 3/5 stress matrix reported FAIL"
fi

if [ -n "$VERIFIED_GIB" ]; then
    grep -q "NVME-VERIFIED: PASS" "$LOG" ||
        fail "cumulative verified writes did not report PASS"
fi

if [ "$CRASH_RUN" = 1 ]; then
    grep -q "NVME-CRASH-WRITE: PASS" "$LOG" ||
        fail "crash write did not report PASS"
    if [ "$CRASH_BOOT" -gt 1 ]; then
        grep -q "NVME-CRASH-VERIFY: PASS" "$LOG" ||
            fail "crash verification of the previous power cut failed"
    fi
fi

if [ "$BENCH" = 1 ]; then
    grep -q "NVME-BENCH: " "$LOG" || fail "benchmark produced no NVME-BENCH marker"
    if [ "$BENCH_SWEEP" = 1 ] && ! grep -q "NVME-SCALE: PASS" "$LOG"; then
        fail "benchmark scaling did not reach --min-scale=$MIN_SCALE"
    fi
    if [ "$BENCH_COMPARE" = 1 ]; then
        grep -q "NVME-BENCH: dev=/dev/sata02" "$LOG" ||
            fail "AHCI comparison benchmark marker missing"
    fi
fi

# Verify the completion mode actually chosen matches what was requested.
# QEMU's NVMe model only implements MSI-X and INTx (no MSI capability), so an
# MSI request legitimately falls through to the next rung.
case "$IRQ_MODE" in
"") ;;
msix) grep -q "NVME-IRQ: .* mode=msix" "$LOG" || fail "MSI-X mode not reported" ;;
msi)
    if ! grep -q "NVME-IRQ: .* mode=msi" "$LOG"; then
        if grep -q "NVME-IRQ: .* mode=intx" "$LOG"; then
            echo "[*] Note: device has no MSI capability; fell back to INTx"
        else
            fail "MSI mode not reported (and no INTx fallback)"
        fi
    fi
    ;;
intx) grep -q "NVME-IRQ: .* mode=intx" "$LOG" || fail "INTx mode not reported" ;;
poll) grep -q "NVME-IRQ: .* mode=poll" "$LOG" || fail "polled mode not reported" ;;
esac

if [ "$FAULT_TIMEOUT" = 1 ]; then
    if grep -q "NVME-FAILSTOP: FAIL" "$LOG"; then
        fail "forced-timeout fail-stop self-test reported FAIL"
    fi
    grep -q "NVME-FAILSTOP: PASS" "$LOG" ||
        fail "NVME-FAILSTOP marker missing (built with NVME_SELFTEST_FAULT_TIMEOUT=1?)"
fi

if [ "$SELFTEST" = 1 ]; then
    if grep -q "NVME-SELFTEST: FAIL" "$LOG"; then
        fail "kernel NVMe self-test reported FAIL"
    fi
    if ! grep -q "NVME-SELFTEST: PASS" "$LOG"; then
        fail "kernel NVMe self-test marker missing (built with NVME_SELFTEST=1?)"
    fi
    grep -q "NVME-PRPCHAIN: PASS" "$LOG" || fail "PRP chain self-test not PASS"
    grep -q "NVME-SHN: PASS" "$LOG" || fail "graceful SHN self-test not PASS"
    grep -q "NVME-AUDIT: PASS" "$LOG" ||
        fail "Phase 7 hardening audit self-test not PASS"
    if [ "$DATA_SELFTEST" = 1 ]; then
        if [ "$FOURK" = 1 ]; then
            grep -q "NVME-4KN: PASS" "$LOG" || fail "4Kn self-test not PASS"
        fi
        if [ "$MULTI_NS" = 1 ]; then
            grep -q "NVME-MULTINS: PASS" "$LOG" || fail "multi-NS self-test not PASS"
        fi
        if [ "$RECOVERY" = 1 ]; then
            grep -q "NVME-RECOVERY: PASS" "$LOG" || fail "recovery self-test not PASS"
        fi
    fi
fi

# The reboot/power-off syscall path must have asked the controller for a
# graceful shutdown before QEMU exited.
grep -q "NVME-SHUTDOWN: nvme0 SHST=complete" "$LOG" ||
    fail "graceful CC.SHN marker missing at power-off/reboot"

# A detached namespace must not become a block device.
if [ -n "$DETACHED_NSID" ]; then
    if grep -qE "nsid=${DETACHED_NSID}([^0-9]|$)" "$LOG"; then
        fail "detached namespace nsid=$DETACHED_NSID was announced by the driver"
    fi
fi

# Host-side verification: LE64 pattern, guest SHA-256 markers and the 4Kn vs
# 512e pair comparison, all through the device map.
if command -v python3 >/dev/null 2>&1; then
    if ! python3 - "$MAP_FILE" "$LOG" "$PATTERN_IMAGE" "$PATTERN_BASE" \
        "$BENCH_SWEEP" "$MIN_SCALE" <<'PY'
import hashlib
import os
import re
import struct
import sys

map_path, log_path, pattern_img, pattern_base, want_scale, min_scale = (
    sys.argv[1:7]
)
want_scale = want_scale == "1"
min_scale = float(min_scale)
maps = {}
if os.path.exists(map_path):
    with open(map_path) as mf:
        for line in mf:
            parts = line.split()
            if len(parts) == 3:
                maps[parts[0]] = (parts[1], int(parts[2]))

with open(log_path, "rb") as f:
    log = f.read().decode("utf-8", "replace")

failed = False


def fail(msg):
    global failed
    print(msg)
    failed = True


# Legacy byte pattern on the primary scratch partition.
if pattern_img:
    checked = 2048  # first 1 MiB
    base = int(pattern_base)
    with open(pattern_img, "rb") as f:
        f.seek(base)
        bad = None
        for i in range(checked):
            sec = f.read(512)
            if len(sec) < 512:
                bad = f"short read at sector {i}"
                break
            if sec[:8] != struct.pack("<Q", i):
                bad = f"pattern mismatch at sector {i}"
                break
    if bad:
        fail(f"host pattern check FAILED: {bad}")
    else:
        print(f"host pattern check OK ({checked} sectors)")


# Guest SHA-256 markers, mapped to the host backing image.
hits = re.findall(
    r"NVME-HASH: dev=(\S+) offset=(\d+) bytes=(\d+) sha256=([0-9a-f]{64})", log
)
if not hits:
    print("guest hash marker not found; skipping host SHA-256 checks")
for dev, offset, length, want in hits:
    name = dev.rsplit("/", 1)[-1]
    if name not in maps:
        print(f"guest hash device {dev} not mapped; skipping")
        continue
    img, base = maps[name]
    start = base + int(offset)
    length = int(length)
    if start + length > os.path.getsize(img):
        fail(f"guest hash range {start}+{length} exceeds {img}")
        continue
    with open(img, "rb") as f:
        f.seek(start)
        data = f.read(length)
    got = hashlib.sha256(data).hexdigest()
    if got != want:
        fail(f"host SHA-256 mismatch for {dev}: guest={want} host={got}")
    else:
        print(f"host SHA-256 OK ({dev} offset={offset} bytes={length})")


# 4Kn vs 512e pair: both devices must hold identical bytes.
for m in re.finditer(
    r"NVME-PAIR: (\S+) devA=(\S+) devB=(\S+) bytes=(\d+)", log
):
    status, a, b, n = m.group(1), m.group(2), m.group(3), int(m.group(4))
    if status != "PASS":
        fail(f"guest pair test reported {status} ({a} vs {b})")
        continue
    na, nb = a.rsplit("/", 1)[-1], b.rsplit("/", 1)[-1]
    if na not in maps or nb not in maps:
        print(f"pair devices {a}/{b} not mapped; skipping host compare")
        continue
    ia, ba = maps[na]
    ib, bb = maps[nb]
    if os.path.getsize(ia) < ba + n or os.path.getsize(ib) < bb + n:
        fail("pair compare range exceeds a backing image")
        continue
    with open(ia, "rb") as fa, open(ib, "rb") as fb:
        fa.seek(ba)
        fb.seek(bb)
        remaining = n
        same = True
        while remaining > 0:
            chunk = min(remaining, 1 << 20)
            if fa.read(chunk) != fb.read(chunk):
                same = False
                break
            remaining -= chunk
    if same:
        print(f"host pair compare OK ({a} == {b}, {n} bytes)")
    else:
        fail(f"host pair compare mismatch between {a} and {b}")


# Phase 6 benchmark results: report every measurement and independently check
# the qd4/qd1 scaling the guest claims.
bench = {}
for m in re.finditer(
    r"NVME-BENCH: dev=(\S+) mode=(\S+) block=(\d+) qd=(\d+) "
    r"seconds=([\d.]+) iops=([\d.]+) mbps=([\d.]+)",
    log,
):
    dev, mode, block, qd, secs, iops, mbps = m.groups()
    bench.setdefault(dev, {})[int(qd)] = (float(iops), float(mbps))
    print(
        f"bench {dev} mode={mode} block={block} qd={qd} "
        f"iops={float(iops):.0f} mbps={float(mbps):.1f}"
    )

if want_scale:
    checked = 0
    for dev, results in bench.items():
        if 1 not in results or 4 not in results:
            continue
        iops1 = results[1][0]
        iops4 = results[4][0]
        ratio = iops4 / iops1 if iops1 > 0 else 0.0
        checked += 1
        if ratio >= min_scale:
            print(f"host scale check OK {dev}: qd4/qd1={ratio:.2f}")
        else:
            fail(
                f"host scale check FAILED {dev}: qd4/qd1={ratio:.2f} "
                f"< {min_scale:.2f}"
            )
    if checked == 0 and not failed:
        fail("host scale check: no device produced qd=1 and qd=4 results")

# NVMe vs AHCI comparison summary (informational; both must have run).
if "/dev/sata02" in bench and len(bench) > 1:
    ahci = bench["/dev/sata02"]
    for dev, results in bench.items():
        if dev == "/dev/sata02":
            continue
        for qd in sorted(set(results) & set(ahci)):
            nvme = results[qd]
            a = ahci[qd]
            print(
                f"NVMe vs AHCI ({dev} qd={qd}): nvme={nvme[1]:.1f} MiB/s "
                f"ahci={a[1]:.1f} MiB/s"
            )
            break

sys.exit(1 if failed else 0)
PY
    then
        fail "host verification failed (see reasons above)"
    fi
else
    echo "[*] host verification skipped (python3 missing)"
fi

# --- e2fsck ----------------------------------------------------------------

fsck_copy_root() {
    dd if="$TEST_IMG" of="$ROOT_COPY" bs=512 skip="$P1_START" \
        count="${P1_SECTORS:-$((1536 * 2048))}" status=none
}

if [ "$FSCK" = 1 ] && [ "$CRASH_RUN" = 1 ]; then
    # After a power cut the journal may need replay: fix a copy, then require
    # a clean read-only check.
    if command -v e2fsck >/dev/null 2>&1 && [ -f build/nvme/nvme_test.layout ]; then
        fsck_copy_root
        rc=0
        e2fsck -fy "$ROOT_COPY" >"$LOG_DIR/e2fsck.log" 2>&1 || rc=$?
        if [ "$rc" -ge 4 ]; then
            tail -n 20 "$LOG_DIR/e2fsck.log" >&2 || true
            fail "host e2fsck -fy left uncorrected errors after crash $CRASH_BOOT"
        fi
        if ! e2fsck -fn "$ROOT_COPY" >>"$LOG_DIR/e2fsck.log" 2>&1; then
            tail -n 20 "$LOG_DIR/e2fsck.log" >&2 || true
            fail "host e2fsck -fn not clean after crash journal replay"
        fi
        echo "[*] host e2fsck after crash $CRASH_BOOT OK (journal replay, rc=$rc)"
        rm -f "$ROOT_COPY"
    else
        echo "[*] host e2fsck skipped (e2fsck or layout file missing)"
    fi
elif [ "$FSCK" = 1 ] && [ "$CONFIG" = "nvme-root" ]; then
    if command -v e2fsck >/dev/null 2>&1 && [ -f build/nvme/nvme_test.layout ]; then
        fsck_copy_root
        if e2fsck -fn "$ROOT_COPY" >"$LOG_DIR/e2fsck.log" 2>&1; then
            echo "[*] host e2fsck -fn OK (root partition)"
        else
            tail -n 20 "$LOG_DIR/e2fsck.log" >&2 || true
            fail "host e2fsck -fn reported errors on the root partition"
        fi
        rm -f "$ROOT_COPY"
    else
        echo "[*] host e2fsck skipped (e2fsck or layout file missing)"
    fi
fi

if [ "$SELFTEST" = 1 ]; then
    echo "[*] Note: the kernel is still built with NVME_SELFTEST=1."
    echo "    Restore a release build with: make -C kernel clean && make avoryos-x86_64.iso"
fi

echo "[*] RESULT: PASS (log $LOG)"
