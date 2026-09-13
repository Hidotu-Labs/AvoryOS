#!/usr/bin/env bash
# linux-firmware-install.sh -- stage the firmware images AvoryOS ships.
#
# Fetches (or reuses) a linux-firmware checkout and copies the files/globs in
# scripts/linux/firmware-manifest.txt into build/firmware/lib/firmware/.  The
# top-level disk image build installs that directory at /lib/firmware, which is
# where the LinuxKPI request_firmware() loader looks (Phase 6 uses it for the
# amdgpu PSP/GC/SDMA/VCN/DMCUB blobs).
#
# The same pass writes build/firmware/kpi_fw_manifest.h: a C table with the
# staged name, byte size and zlib CRC32 of every blob, consumed by the Phase 5
# firmware self-test to prove the disk image carries exactly what was staged.
#
# Usage:
#   scripts/linux-firmware-install.sh            # stage into build/firmware/
#   LINUX_FIRMWARE_REF=<sha> scripts/linux-firmware-install.sh
#   LINUX_FIRMWARE_SRC=/lib/firmware scripts/linux-firmware-install.sh
#
# Environment:
#   LINUX_FIRMWARE_REPO  git remote (default: kernel.org linux-firmware)
#   LINUX_FIRMWARE_REF   commit/tag to check out (default: pinned below)
#   LINUX_FIRMWARE_SRC   checkout cache (default: build/linux-firmware); a
#                        plain directory without .git is used as-is, which is
#                        how the host's /lib/firmware tree can be staged for
#                        local development.  .zst/.xz members are decompressed
#                        on the way in, so the image only carries plain blobs.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
REPO="${LINUX_FIRMWARE_REPO:-https://git.kernel.org/pub/scm/linux/kernel/git/firmware/linux-firmware.git}"
# Pinned for reproducible builds (the Phase 6 C4 evidence names this ref).
REF="${LINUX_FIRMWARE_REF:-20260910}"
SRC="${LINUX_FIRMWARE_SRC:-$ROOT/build/linux-firmware}"
DEST="$ROOT/build/firmware/lib/firmware"
MANIFEST="$ROOT/scripts/linux/firmware-manifest.txt"
GEN="$ROOT/build/firmware/kpi_fw_manifest.h"

log() { printf '[linux-firmware] %s\n' "$*"; }
die() { printf '[linux-firmware] error: %s\n' "$*" >&2; exit 1; }

[ -f "$MANIFEST" ] || die "missing $MANIFEST"

# Collect active manifest entries.
mapfile -t entries < <(grep -v '^[[:space:]]*\(#\|$\)' "$MANIFEST")
if [ "${#entries[@]}" -eq 0 ]; then
    log "manifest has no active entries; nothing to install"
    log "add files to scripts/linux/firmware-manifest.txt when a driver needs them"
    rm -f "$GEN"
    exit 0
fi

if [ -d "$SRC/.git" ]; then
    if [ -n "$REF" ]; then
        log "fetching $REF"
        git -C "$SRC" fetch --depth 1 origin "$REF"
        git -C "$SRC" checkout --detach FETCH_HEAD
    fi
elif [ -d "$SRC" ]; then
    log "using plain firmware tree at $SRC (no git metadata)"
else
    log "cloning $REPO at ${REF:-master} (this is several hundred MB)"
    mkdir -p "$(dirname "$SRC")"
    if [ -n "$REF" ]; then
        git clone --depth 1 --branch "$REF" "$REPO" "$SRC"
    else
        git clone --depth 1 "$REPO" "$SRC"
    fi
fi

mkdir -p "$DEST"
staged_list="$(mktemp)"
trap 'rm -f "$staged_list"' EXIT
copied=0

# Stage one match.  $1 is the path relative to the source tree (possibly
# compressed); $2 is the plain destination name relative to $DEST.
install_one() {
    local src_name="$1" out_name="$2"

    mkdir -p "$DEST/$(dirname "$out_name")"
    case "$src_name" in
        *.zst)
            command -v unzstd >/dev/null 2>&1 ||
                die "unzstd is required to stage $src_name"
            # Read through stdin: distro trees symlink identical blobs and
            # zstd refuses symlink arguments.
            unzstd -c < "$SRC/$src_name" > "$DEST/$out_name" ;;
        *.xz)
            command -v unxz >/dev/null 2>&1 ||
                die "unxz is required to stage $src_name"
            unxz -c < "$SRC/$src_name" > "$DEST/$out_name" ;;
        *)
            cp -f "$SRC/$src_name" "$DEST/$out_name" ;;
    esac
    printf '%s\n' "$out_name" >> "$staged_list"
    copied=$((copied + 1))
}

for entry in "${entries[@]}"; do
    found=0
    # Expand the entry as a glob, accepting compressed members too.  Plain
    # names win when both exist: the plain pass runs first and duplicates
    # (same destination) are skipped.
    for suffix in "" ".zst" ".xz"; do
        while IFS= read -r m; do
            [ -n "$m" ] || continue
            out="$m"
            out="${out%.zst}"
            out="${out%.xz}"
            if grep -qxF "$out" "$staged_list"; then
                continue
            fi
            install_one "$m" "$out"
            found=$((found + 1))
        done < <(cd "$SRC" && compgen -G "$entry$suffix" || true)
    done

    if [ "$found" -eq 0 ]; then
        die "manifest entry matches nothing: $entry"
    fi
done

# Emit the C manifest used by the Phase 5 firmware self-test.  zlib.crc32()
# matches the kernel's crc32_le(~0, p, len) ^ ~0.
if [ -d "$SRC/.git" ]; then
    ref_line="${REF:-master}"
else
    ref_line="(plain tree; pin not enforced)"
fi
mkdir -p "$(dirname "$GEN")"
{
    printf '/* Generated by scripts/linux-firmware-install.sh -- do not edit. */\n'
    printf '/* linux-firmware ref: %s ; source: %s */\n' "$ref_line" "$SRC"
    printf '#ifndef KPI_FW_MANIFEST_H\n'
    printf '#define KPI_FW_MANIFEST_H\n\n'
    printf 'struct kpi_fw_manifest_entry {\n'
    printf '  const char *name;\n'
    printf '  unsigned int size;\n'
    printf '  unsigned int crc32;\n'
    printf '};\n\n'
    printf 'static const struct kpi_fw_manifest_entry kpi_fw_manifest[] = {\n'
    python3 - "$DEST" "$staged_list" <<'PY'
import os
import sys
import zlib

dest, list_path = sys.argv[1], sys.argv[2]
with open(list_path) as names:
    for line in names:
        name = line.strip()
        if not name:
            continue
        with open(os.path.join(dest, name), "rb") as blob:
            data = blob.read()
        print('  {"%s", %du, 0x%08xu},' % (name, len(data), zlib.crc32(data) & 0xffffffff))
PY
    printf '};\n\n'
    printf '#define KPI_FW_MANIFEST_COUNT (sizeof(kpi_fw_manifest) / sizeof(kpi_fw_manifest[0]))\n\n'
    printf '#endif /* KPI_FW_MANIFEST_H */\n'
} > "$GEN"

log "staged $copied file(s) into build/firmware/lib/firmware/"
log "wrote $GEN"
