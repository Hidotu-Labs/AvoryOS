#!/usr/bin/env bash
# create-nvme-test.sh - build the headless NVMe test image.
#
# Outputs (in the repository root):
#   nvme_test.img    MBR disk, partition 1 = minimal ext4 root, partition 2 =
#                    raw scratch used by /bin/nvme_test auto.
#   nvme_scratch.img blank 512 MiB disk for whole-device raw I/O tests.
#
# The minimal root contains a small /sbin/init launcher, the NVMe test tools,
# and a generated command file so scripts/nvme/nvme-stress.sh can run headless.
#
# Layout constants are recorded in build/nvme/nvme_test.layout for the host-side
# pattern verification step.

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT_DIR"

TEST_IMG="${TEST_IMG:-nvme_test.img}"
SCRATCH_IMG="${SCRATCH_IMG:-nvme_scratch.img}"
BUILD_DIR="build/nvme"
ROOT_IMG="$BUILD_DIR/nvme-root.img"

IMG_MIB="${IMG_MIB:-4096}"
ROOT_MIB="${ROOT_MIB:-1536}"
SCRATCH_MIB="${SCRATCH_MIB:-512}"

P1_START=2048
P1_SECTORS=$((ROOT_MIB * 2048))
P2_START=$((P1_START + P1_SECTORS))
P2_SECTORS=$((IMG_MIB * 2048 - P2_START))

mkdir -p "$BUILD_DIR"

for f in userland/nvme_init.elf userland/shutdown.elf userland/nvme_test.elf \
    userland/nvme_bench.elf; do
    if [ ! -f "$f" ]; then
        echo "create-nvme-test.sh: missing $f (build the kernel/userland first)" >&2
        exit 1
    fi
done

for tool in mkfs.ext4 debugfs sfdisk truncate; do
    if ! command -v "$tool" >/dev/null 2>&1; then
        echo "create-nvme-test.sh: required tool '$tool' not found" >&2
        exit 1
    fi
done

echo "[*] Building minimal ext4 root (${ROOT_MIB} MiB)..."
rm -f "$ROOT_IMG"
truncate -s "${ROOT_MIB}M" "$ROOT_IMG"
mkfs.ext4 -q -F -b 1024 -I 128 \
    -O extent,filetype,has_journal,dir_index,^64bit,^metadata_csum,^flex_bg,^huge_file,^dir_nlink,^extra_isize,^metadata_csum_seed,^orphan_file \
    "$ROOT_IMG"

# The init launcher executes this whitespace-separated command as PID 1.
# Commands are assembled by nvme-stress.sh from fixed executable paths and
# numeric/enum options, so shell quoting and expansion are intentionally absent.
printf '%s %s\n' \
    "${TEST_CMD:-/bin/nvme_test auto}" "${NVME_TEST_ARGS:-}" \
    > "$BUILD_DIR/nvme-test.command"

{
    echo "cd /"
    echo "mkdir bin"
    echo "mkdir sbin"
    echo "mkdir dev"
    echo "mkdir etc"
    echo "mkdir proc"
    echo "mkdir run"
    echo "mkdir sys"
    echo "mkdir tmp"
    echo "write userland/nvme_init.elf sbin/init"
    echo "write $BUILD_DIR/nvme-test.command etc/nvme-test.command"
    echo "write userland/shutdown.elf bin/shutdown"
    echo "write userland/nvme_test.elf bin/nvme_test"
    echo "write userland/nvme_bench.elf bin/nvme_bench"
    echo "set_inode_field sbin/init mode 0100755"
    echo "set_inode_field bin/shutdown mode 0100755"
    echo "set_inode_field bin/nvme_test mode 0100755"
    echo "set_inode_field bin/nvme_bench mode 0100755"
} | debugfs -w "$ROOT_IMG" >/dev/null 2>&1 || true

echo "[*] Creating $TEST_IMG (${IMG_MIB} MiB: root ${ROOT_MIB} MiB + scratch $(( (P2_SECTORS + 2047) / 2048 )) MiB)..."
rm -f "$TEST_IMG"
dd if=/dev/zero of="$TEST_IMG" bs=1M count="$IMG_MIB" status=none
{
    echo "label: dos"
    echo "${P1_START},${P1_SECTORS},83,*"
    echo "${P2_START},${P2_SECTORS},83"
} | sfdisk "$TEST_IMG" >/dev/null 2>&1
dd if="$ROOT_IMG" of="$TEST_IMG" bs=512 seek="$P1_START" conv=notrunc status=none
rm -f "$ROOT_IMG"

echo "[*] Creating $SCRATCH_IMG (${SCRATCH_MIB} MiB, blank)..."
dd if=/dev/zero of="$SCRATCH_IMG" bs=1M count="$SCRATCH_MIB" status=none

cat > "$BUILD_DIR/nvme_test.layout" <<EOF
TEST_IMG=$TEST_IMG
SCRATCH_IMG=$SCRATCH_IMG
P1_START=$P1_START
P1_SECTORS=$P1_SECTORS
P2_START=$P2_START
P2_SECTORS=$P2_SECTORS
EOF

echo "[*] Done. Layout: $BUILD_DIR/nvme_test.layout"
