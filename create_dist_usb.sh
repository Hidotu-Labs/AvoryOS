#!/bin/bash
set -e

# Distribution ISO Generator for AvoryOS
# Produces a self-contained bootable hybrid ISO (BIOS + UEFI).
# disk.img is shrunk and embedded as a Limine module; the kernel's ramdisk
# driver picks it up and mounts it as root — no separate drive needed.
#
# Usage: ./create_dist_usb.sh [output.iso]
#
# Env overrides (used by the minimal ISO build):
#   DISK_IMG=path     source disk image (default: disk.img)
#   LIMINE_CONF=path  Limine config to embed (default: limine.conf)

ARCH="x86_64"
OUT="${1:-avoryos-dist.iso}"
# Overridable so the minimal image (build/minimal/disk.img +
# limine-minimal.conf) can reuse this script; defaults keep the full dist
# image behavior unchanged.
DISK_IMG="${DISK_IMG:-disk.img}"
LIMINE_CONF="${LIMINE_CONF:-limine.conf}"
ISO_ROOT="iso_root_dist"
TMPPART=""

cleanup() {
    rm -f "$TMPPART"
    rm -rf "$ISO_ROOT"
}
trap cleanup EXIT

# ── Dependency checks ────────────────────────────────────────────────────────
if ! command -v xorriso &>/dev/null; then
    echo "[ERROR] 'xorriso' is not installed."
    exit 1
fi

# ── Pre-flight checks ────────────────────────────────────────────────────────
for f in "kernel/bin-${ARCH}/kernel" "$DISK_IMG" limine/limine "$LIMINE_CONF"; do
    if [ ! -f "$f" ]; then
        echo "[ERROR] Required file not found: $f"
        exit 1
    fi
done

# ── Shrink disk.img to actual used size ──────────────────────────────────────
# The ext4 partition inside disk.img typically occupies ~900MB of a 2GB image.
# We extract the partition, run resize2fs -M to shrink it to minimum, then
# reassemble with the original MBR so the kernel can still read the MBR layout.
TMPPART=$(mktemp /tmp/avoryos-part-XXXXXX.img)
SHRUNK_DISK="${ISO_ROOT}/disk.img"   # written directly into the ISO root

echo "[INFO] Extracting partition from ${DISK_IMG}..."
# Partition 1 starts at sector 2048 (standard 1 MiB MBR gap)
dd if="$DISK_IMG" of="$TMPPART" bs=512 skip=2048 status=none

echo "[INFO] Checking filesystem..."
e2fsck -fy "$TMPPART" >/dev/null 2>&1 || true

echo "[INFO] Shrinking ext4 to minimum..."
resize2fs -M "$TMPPART" 2>&1 | grep -E 'blocks|The filesystem'

# resize2fs -M leaves the filesystem completely full, so the first runtime
# write (Xorg logs, /tmp, service caches) fails with ENOSPC - the exact
# "alloc_extent FAILED ... reason=2" reports on real hardware.  Grow the image
# back by a fixed slack and let ext4 expand into it, so the shipped system has
# working room on its root filesystem.
SLACK_MB="${AVORYOS_DISK_SLACK_MB:-64}"
MIN_BYTES=$(wc -c < "$TMPPART")
TARGET_BYTES=$(( MIN_BYTES + SLACK_MB * 1024 * 1024 ))
echo "[INFO] Adding ${SLACK_MB} MiB free-space slack to the root filesystem..."
truncate -s "$TARGET_BYTES" "$TMPPART"
e2fsck -fy "$TMPPART" >/dev/null 2>&1 || true
resize2fs "$TMPPART" >/dev/null 2>&1

NEW_PART_BYTES=$(wc -c < "$TMPPART")
MiB=$(( 1024 * 1024 ))
PART_OFFSET=$(( 2048 * 512 ))   # 1 MiB
NEW_TOTAL=$(( ( (PART_OFFSET + NEW_PART_BYTES + MiB - 1) / MiB ) * MiB ))

ORIG_MiB=$(( $(wc -c < "$DISK_IMG") / MiB ))
NEW_MiB=$(( NEW_TOTAL / MiB ))
echo "[INFO] disk.img: ${ORIG_MiB} MiB -> ${NEW_MiB} MiB"

# ── Build ISO root ───────────────────────────────────────────────────────────
echo "[INFO] Building ISO root..."
rm -rf "$ISO_ROOT"
mkdir -p "$ISO_ROOT/boot/limine"
mkdir -p "$ISO_ROOT/EFI/BOOT"

# Assemble the shrunk disk image directly into the ISO staging dir
echo "[INFO] Writing shrunk disk image into ISO root..."
dd if="$DISK_IMG" of="$SHRUNK_DISK" bs=512 count=2048 status=none
dd if="$TMPPART" of="$SHRUNK_DISK" bs=512 seek=2048  status=none conv=notrunc
rm -f "$TMPPART"; TMPPART=""

# The partition entry copied from the source MBR still describes the original,
# larger partition.  The kernel's partition scan rejects an entry that runs
# past the end of the device, so root mounting falls through to the raw
# ramdisk (which starts with the MBR, not ext4) and the boot drops to the
# kernel shell.  Patch the entry to the shrunk size; this is what makes the
# embedded disk image mountable at all.
NEW_PART_SECTORS=$(( (NEW_PART_BYTES + 511) / 512 ))
echo "[INFO] Patching MBR partition 1 length to ${NEW_PART_SECTORS} sectors..."
python3 - "$SHRUNK_DISK" "$NEW_PART_SECTORS" <<'PY'
import struct
import sys

path, sectors = sys.argv[1], int(sys.argv[2])
with open(path, "r+b") as f:
    f.seek(446)
    entry = bytearray(f.read(16))
    if entry[4] == 0:
        raise SystemExit(f"{path}: partition 1 entry is empty")
    struct.pack_into("<I", entry, 12, sectors)
    f.seek(446)
    f.write(entry)
PY

# Kernel (stripped; the unstripped build ELF stays in kernel/bin-${ARCH}/)
cp -v "kernel/bin-${ARCH}/kernel"   "$ISO_ROOT/boot/"
strip --strip-all "$ISO_ROOT/boot/kernel"

# Limine loads the embedded disk image as a boot module. The kernel's ramdisk
# driver locates it by the module string/path and exposes its first partition.
# Physical hardware should use the firmware/display preferred mode.
# Strip the QEMU-only fixed resolution from the distributable image.
sed '/^[[:space:]]*interface_resolution:/d; /^[[:space:]]*resolution:/d' \
    "$LIMINE_CONF" > "$ISO_ROOT/boot/limine/limine.conf"

[ -f assets/boo.png ] && cp -v assets/boo.png "$ISO_ROOT/boot/limine/"
cp -v limine/limine-bios.sys         "$ISO_ROOT/boot/limine/"
cp -v limine/limine-bios-cd.bin      "$ISO_ROOT/boot/limine/"
cp -v limine/limine-uefi-cd.bin      "$ISO_ROOT/boot/limine/"
cp -v limine/BOOTX64.EFI             "$ISO_ROOT/EFI/BOOT/"
cp -v limine/BOOTIA32.EFI            "$ISO_ROOT/EFI/BOOT/" 2>/dev/null || true

echo "[INFO] Limine config for dist ISO:"
grep -A5 "^/AvoryOS" "$ISO_ROOT/boot/limine/limine.conf"

# ── Generate the ISO ─────────────────────────────────────────────────────────
echo "[INFO] Running xorriso..."
xorriso -as mkisofs -R -r -J \
    -b boot/limine/limine-bios-cd.bin \
    -no-emul-boot -boot-load-size 4 -boot-info-table \
    -hfsplus -apm-block-size 2048 \
    --efi-boot boot/limine/limine-uefi-cd.bin \
    -efi-boot-part --efi-boot-image \
    --protective-msdos-label \
    "$ISO_ROOT" -o "$OUT"

echo "[INFO] Installing Limine BIOS boot code..."
./limine/limine bios-install "$OUT"

# ── Done ─────────────────────────────────────────────────────────────────────
ISO_SIZE=$(du -h "$OUT" | cut -f1)
echo ""
echo "[SUCCESS] $OUT  ($ISO_SIZE)  — self-contained, boot from ISO alone."
echo ""
echo "  Test in QEMU (UEFI):"
echo "    qemu-system-x86_64 \\"
echo "      -drive if=pflash,unit=0,format=raw,file=edk2-ovmf/ovmf-code-x86_64.fd,readonly=on \\"
echo "      -cdrom \"$OUT\" -m 2G -serial stdio"
echo ""
echo "  Write to USB:"
echo "    sudo dd if=\"$OUT\" of=/dev/sdX bs=4M status=progress conv=fsync"
