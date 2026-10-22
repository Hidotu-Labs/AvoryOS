#!/usr/bin/env bash
# linux-import.sh -- vendor the pinned upstream Linux subset used by AvoryOS.
#
# AvoryOS does not modify the upstream Linux tree.  This script shallow-clones
# a pinned stable tag into build/linux-src and copies the paths listed in
# scripts/linux/subset.txt into kernel/linux/ (gitignored).  It also writes
# kernel/linux/Makefile.files, which kernel/GNUmakefile includes to learn which
# upstream .c files to compile, and links the checked-in generated kernel
# config (kernel/linuxkpi/include/generated/) into the tree where Linux
# headers expect it (<generated/autoconf.h>).
#
# Usage:
#   scripts/linux-import.sh                 # use the checked-in baseline
#   LINUX_VERSION=v6.6.62 scripts/linux-import.sh
#   scripts/linux-import.sh --force         # re-copy even if the pin matches
#
# Environment:
#   LINUX_VERSION     tag to import (default: scripts/linux/baseline.txt)
#   LINUX_REPO        git remote (default: kernel.org stable tree)
#   LINUX_SRC_DIR     clone cache (default: build/linux-src)
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
LINUX_REPO="${LINUX_REPO:-https://git.kernel.org/pub/scm/linux/kernel/git/stable/linux.git}"
BASELINE_FILE="$ROOT/scripts/linux/baseline.txt"
DEFAULT_TAG="$(sed -n 's/^tag=//p' "$BASELINE_FILE")"
DEFAULT_COMMIT="$(sed -n 's/^commit=//p' "$BASELINE_FILE")"
LINUX_VERSION="${LINUX_VERSION:-$DEFAULT_TAG}"
SRC_DIR="${LINUX_SRC_DIR:-$ROOT/build/linux-src}"
DEST="$ROOT/kernel/linux"
SUBSET_FILE="$ROOT/scripts/linux/subset.txt"
FILES_FILE="$ROOT/scripts/linux/files.txt"
GEN_SRC="$ROOT/kernel/linuxkpi/include/generated"
FORCE=0
NOUVEAU_SERIES=0

for arg in "$@"; do
    case "$arg" in
        --force) FORCE=1 ;;
        -h|--help) sed -n '2,25p' "$0"; exit 0 ;;
        *) echo "linux-import: unknown argument: $arg" >&2; exit 2 ;;
    esac
done

log()  { printf '[linux-import] %s\n' "$*"; }
die()  { printf '[linux-import] error: %s\n' "$*" >&2; exit 1; }

exec 9>"$ROOT/kernel/.linux-import.lock"
flock -n 9 || die "another Linux import is active"

# Read "VAR += a b c" entries from a Kbuild fragment (one entry per line; the
# fragments we consume do not use line continuations for these variables).
collect() {
    sed -n "s/^[[:space:]]*$2[[:space:]]*+=[[:space:]]*//p" "$1" | tr ' ' '\n' | sed '/^$/d'
}

# Reproduce Kbuild's asm-generic wrapper generation (scripts/Makefile.asm-generic):
# for each header in the arch's generic-y plus the generic mandatory-y that the
# arch does not implement, emit a wrapper that includes <asm-generic/NAME>.
# Wrappers go to arch/x86/include/generated/(uapi/)asm/, which is on the
# imported-code include path.  This is how <asm/types.h>, <asm/io.h> and
# friends resolve for x86.
gen_asm_wrappers() {
    local generic_kbuild="$1" arch_kbuild="$2" arch_dir="$3" out_dir="$4"
    [ -f "$arch_kbuild" ] || die "missing Kbuild fragment: $arch_kbuild"
    [ -f "$generic_kbuild" ] || die "missing Kbuild fragment: $generic_kbuild"

    local generated_list
    generated_list="$(collect "$arch_kbuild" generated-y)"

    local f
    {
        collect "$arch_kbuild" generic-y
        collect "$generic_kbuild" mandatory-y
    } | sort -u | while IFS= read -r f; do
        if printf '%s\n' "$generated_list" | grep -qx "$f"; then
            continue
        fi
        if [ -e "$arch_dir/$f" ]; then
            continue
        fi
        mkdir -p "$out_dir"
        printf '#include <asm-generic/%s>\n' "$f" > "$out_dir/$f"
    done
}

[ -f "$SUBSET_FILE" ] || die "missing $SUBSET_FILE"
[ -f "$FILES_FILE" ]  || die "missing $FILES_FILE"

# ── Resolve the tag ──────────────────────────────────────────────────────────
PIN_FILE="$DEST/.pin"
TAG=""
if [ "$FORCE" -eq 0 ] && [ "$LINUX_VERSION" = auto ] && [ -f "$PIN_FILE" ]; then
    TAG="$(sed -n 's/^tag=//p' "$PIN_FILE" | head -1)"
fi
if [ -z "$TAG" ]; then
    if [ "$LINUX_VERSION" != auto ]; then
        TAG="$LINUX_VERSION"
    else
        log "resolving latest v6.6.* stable tag from $LINUX_REPO"
        TAG="$(git ls-remote --tags --refs "$LINUX_REPO" 'v6.6.*' \
               | awk -F/ '{print $NF}' | sort -V | tail -1)"
        [ -n "$TAG" ] || die "could not resolve a v6.6.* tag"
    fi
fi
log "target tag: $TAG"

# ── Fetch the source tree ────────────────────────────────────────────────────
if [ ! -d "$SRC_DIR/.git" ]; then
    log "shallow-cloning $LINUX_REPO @ $TAG"
    mkdir -p "$(dirname "$SRC_DIR")"
    git clone --depth 1 --branch "$TAG" "$LINUX_REPO" "$SRC_DIR"
else
    # Compare the checked-out commit against the requested tag's commit.  The
    # stable tags are tag objects, so 'describe --exact-match' is unreliable;
    # resolve through ^{commit} instead.
    target_commit="$(git -C "$SRC_DIR" rev-parse "$TAG^{commit}" 2>/dev/null || true)"
    head_commit="$(git -C "$SRC_DIR" rev-parse HEAD 2>/dev/null || true)"
    if [ -z "$target_commit" ] || [ "$head_commit" != "$target_commit" ]; then
        log "fetching $TAG into existing checkout at $SRC_DIR"
        git -C "$SRC_DIR" fetch --depth 1 origin "refs/tags/$TAG:refs/tags/$TAG"
        git -C "$SRC_DIR" checkout --detach "$TAG"
    fi
fi
COMMIT="$(git -C "$SRC_DIR" rev-parse "$TAG^{commit}")"
if [ "$TAG" = "$DEFAULT_TAG" ] && [ "$COMMIT" != "$DEFAULT_COMMIT" ]; then
    die "baseline tag resolved to $COMMIT, expected $DEFAULT_COMMIT"
fi
log "commit: $COMMIT"
if [ "$TAG" = v6.18.42 ]; then
    [ "$COMMIT" = 856a9b51680cbbed1ace1207b29e424a616cff48 ] || die "6.18.42 source pin mismatch"
    NOUVEAU_SERIES=1
fi

# Build a complete import before replacing the active one. Failed Kbuild,
# generation or manifest validation must leave the last import intact.
FINAL_DEST="$DEST"
STAGED_DEST="$(mktemp -d "$ROOT/kernel/.linux-import.XXXXXXXX")"
cleanup_import() {
    # Also restore the old tree if interrupted between the two publication
    # renames. Generation failures never reach this window.
    if [ -n "${PREVIOUS_DEST:-}" ] && [ ! -e "$FINAL_DEST" ] && [ -e "$PREVIOUS_DEST" ]; then
        mv "$PREVIOUS_DEST" "$FINAL_DEST"
    fi
    rm -rf -- "$STAGED_DEST"
}
trap cleanup_import EXIT
DEST="$STAGED_DEST"
PIN_FILE="$DEST/.pin"

# ── Copy the subset ──────────────────────────────────────────────────────────
log "copying subset into kernel/linux/"
while IFS= read -r path; do
    case "$path" in ''|\#*) continue ;; esac
    optional=0
    case "$path" in \?*) optional=1; path="${path#?}" ;; esac
    # Entries may contain shell globs (e.g. drivers/gpu/drm/*.h): copy every
    # match.  A literal path that does not exist is an error.
    matched=0
    for src in "$SRC_DIR"/$path; do
        [ -e "$src" ] || continue
        matched=1
        rel="${src#"$SRC_DIR"/}"
        case "$rel" in
            lib/crc32.c) rel=lib/crc/crc32-main.c ;;
            lib/crc32defs.h) rel=lib/crc/crc32defs.h ;;
            drivers/gpu/drm/tiny/simpledrm.c) rel=drivers/gpu/drm/sysfb/simpledrm.c ;;
            drivers/gpu/drm/drm_encoder_slave.c) continue ;;
        esac
        mkdir -p "$DEST/$(dirname "$rel")"
        cp -a "$src" "$DEST/$rel"
    done
    [ "$matched" -eq 1 ] || [ "$optional" -eq 1 ] || die "subset path missing in tree: $path"
done < "$SUBSET_FILE"

if [ "$NOUVEAU_SERIES" -eq 1 ]; then
    for path in drivers/gpu/drm/clients drivers/gpu/drm/sysfb; do
        [ -d "$SRC_DIR/$path" ] || die "6.18 Nouveau profile path missing: $path"
        mkdir -p "$DEST/$(dirname "$path")"
        mkdir -p "$DEST/$path"
        cp -a "$SRC_DIR/$path"/. "$DEST/$path"/
    done
    mkdir -p "$DEST/lib/crc" "$DEST/kernel/time"
    cp -a "$SRC_DIR/lib/crc"/. "$DEST/lib/crc/"
    cp -a "$SRC_DIR/kernel/time/timeconv.c" "$DEST/kernel/time/timeconv.c"
    rm -f "$DEST/drivers/gpu/drm/drm_encoder_slave.c"
fi

# ── Apply AvoryOS divergence patches ─────────────────────────────────────────
# kernel/linux/ is a pristine vendor tree.  The few deliberate divergences the
# build needs live as patch files in scripts/linux/patches/ and are applied on
# every import so a re-import can never silently lose them.  Debug-only
# patches stay under scripts/linux/patches/debug/ and are applied by hand.
PATCH_DIR="$ROOT/scripts/linux/patches"
# Series-specific patches may touch drivers absent from retained baselines.
# v6.6, for example, predates Nouveau's r535 GSP implementation.
PATCH_SERIES="$(printf '%s\n' "$TAG" | cut -d. -f1,2)"
for p in "$PATCH_DIR"/*.patch "$PATCH_DIR/$PATCH_SERIES"/*.patch; do
    [ -e "$p" ] || continue
    if [ "$NOUVEAU_SERIES" -eq 1 ] && [ "$(dirname "$p")" = "$PATCH_DIR" ]; then
        continue
    fi
    log "applying divergence patch: $(basename "$p")"
    patch -p1 -d "$DEST" -s < "$p" || die "divergence patch failed: $p"
done

# ── Generate Makefile.files from scripts/linux/files.txt ─────────────────────
srcfiles=()
while IFS= read -r f; do
    case "$f" in ''|\#*) continue ;; esac
    case "$f" in \?*) f="${f#?}"; [ -f "$SRC_DIR/$f" ] || continue ;; esac
    if [ "$NOUVEAU_SERIES" -eq 1 ]; then
        case "$f" in
            lib/crc32.c) f=lib/crc/crc32-main.c ;;
            lib/crc32defs.h) f=lib/crc/crc32defs.h ;;
            drivers/gpu/drm/drm_encoder_slave.c) continue ;;
        esac
    fi
    srcfiles+=("$f")
done < "$FILES_FILE"
if [ "$NOUVEAU_SERIES" -eq 1 ]; then
    for f in drivers/gpu/drm/drm_client_event.c drivers/gpu/drm/sysfb/drm_sysfb.c drivers/gpu/drm/sysfb/drm_sysfb_modeset.c kernel/time/timeconv.c; do
        [ -f "$DEST/$f" ] && srcfiles+=("$f")
    done
    for f in drivers/gpu/drm/drm_vblank_helper.c drivers/gpu/drm/drm_panel_backlight_quirks.c drivers/gpu/drm/vkms/vkms_config.c drivers/gpu/drm/vkms/vkms_connector.c drivers/gpu/drm/ttm/ttm_backup.c; do
        [ -f "$DEST/$f" ] && srcfiles+=("$f")
    done
    srcfiles=($(printf '%s\n' "${srcfiles[@]}" | awk '!seen[$0]++'))
fi
[ "${#srcfiles[@]}" -gt 0 ] || die "scripts/linux/files.txt lists no source files"
for f in "${srcfiles[@]}"; do
    [ -f "$DEST/$f" ] || die "listed source not in imported tree: $f"
done
{
    echo "# Generated by scripts/linux-import.sh -- do not edit by hand."
    echo "# Source: $LINUX_REPO @ $TAG ($COMMIT)"
    echo "# Configure the file list in scripts/linux/files.txt."
    printf 'linux-obj-y := \\\n'
    last=$((${#srcfiles[@]} - 1))
    for i in "${!srcfiles[@]}"; do
        if [ "$i" -eq "$last" ]; then
            printf '    %s\n' "${srcfiles[$i]}"
        else
            printf '    %s \\\n' "${srcfiles[$i]}"
        fi
    done
} > "$DEST/Makefile.files"
log "wrote kernel/linux/Makefile.files (${#srcfiles[@]} object(s))"

# ── Generated headers expected by Linux headers ──────────────────────────────
mkdir -p "$DEST/include/generated" "$DEST/include/generated/uapi/linux"
for h in "$GEN_SRC"/*.h; do
    [ -e "$h" ] || continue
    ln -sf "../../../linuxkpi/include/generated/$(basename "$h")" \
        "$DEST/include/generated/$(basename "$h")"
done
if [ "$NOUVEAU_SERIES" -eq 1 ]; then
    cfg="$DEST/include/generated/autoconf.h"
    rm -f "$cfg"
    cp "$GEN_SRC/autoconf.h" "$cfg"
    for option in CONFIG_DRM_CLIENT CONFIG_DRM_CLIENT_LIB CONFIG_DRM_SYSFB_HELPER CONFIG_DRM_DISPLAY_DSC_HELPER; do
        grep -q "^#define $option " "$cfg" || sed -i "/^#endif/i #define $option 1" "$cfg"
    done
    awk -v cfg="$cfg" 'BEGIN { while ((getline line < cfg) > 0) if (match(line, /^#define CONFIG_[A-Za-z0-9_]+ [^ ]+/)) { key=substr(line, 9, index(line, " ")-9); value=substr(line, index(line, " ")+1); print key "=" (value == "1" ? "y" : value) } }' > "$DEST/.config"
    awk -f "$SRC_DIR/arch/x86/tools/cpufeaturemasks.awk" \
        "$DEST/arch/x86/include/asm/cpufeatures.h" "$DEST/.config" \
        > "$DEST/arch/x86/include/generated/asm/cpufeaturemasks.h"
fi
# utsrelease.h carries the exact imported tag; write it rather than link it so
# the checked-in tree does not churn on every import.
cat > "$DEST/include/generated/utsrelease.h" <<EOF
/* Generated by scripts/linux-import.sh -- do not edit. */
#define UTS_RELEASE "$TAG"
#define UTS_VERSION "$COMMIT"
EOF
# LINUX_VERSION_CODE from the tag (vMAJ.MIN.PATCH).
ver="${TAG#v}"; maj="${ver%%.*}"; rest="${ver#*.}"; min="${rest%%.*}"; patch="${rest#*.}"
patch="${patch%%-*}"
code=$(( (maj << 16) | (min << 8) | patch ))
cat > "$DEST/include/generated/uapi/linux/version.h" <<EOF
/* Generated by scripts/linux-import.sh -- do not edit. */
#define LINUX_VERSION_CODE $code
#define KERNEL_VERSION(a,b,c) (((a) << 16) + ((b) << 8) + (c))
EOF
# A few headers include <asm/asm-offsets.h>; we have no generated asm offsets
# yet, so satisfy the include with an empty stand-in.
mkdir -p "$DEST/arch/x86/include/generated"
if [ -f "$GEN_SRC/asm-offsets.h" ]; then
    ln -sf "../../../../linuxkpi/include/generated/asm-offsets.h" \
        "$DEST/arch/x86/include/generated/asm-offsets.h"
fi

# <asm/unistd_64.h> is a Kbuild product (scripts/syscallhdr.sh parsing
# arch/x86/entry/syscalls/syscall_64.tbl); generate it so <asm/unistd.h> and
# anything that includes it resolve.  Only the 64-bit/common ABIs are emitted.
if [ -f "$SRC_DIR/arch/x86/entry/syscalls/syscall_64.tbl" ]; then
    mkdir -p "$DEST/arch/x86/include/generated/uapi/asm"
    awk '$2 == "common" || $2 == "64" { printf "#define __NR_%s %s\n", $3, $1 }' \
        "$SRC_DIR/arch/x86/entry/syscalls/syscall_64.tbl" \
        > "$DEST/arch/x86/include/generated/uapi/asm/unistd_64.h"
    awk '$2 == "x32" { printf "#define __NR_%s (0x40000000 + %s)\n", $3, $1 }' \
        "$SRC_DIR/arch/x86/entry/syscalls/syscall_64.tbl" \
        > "$DEST/arch/x86/include/generated/uapi/asm/unistd_64_x32.h"
    awk '$2 == "i386" { printf "#define __NR_%s %s\n", $3, $1 }' \
        "$SRC_DIR/arch/x86/entry/syscalls/syscall_32.tbl" \
        > "$DEST/arch/x86/include/generated/uapi/asm/unistd_32_ia32.h" 2>/dev/null || true
    log "generated arch/x86/include/generated/uapi/asm/unistd_64.h (+x32/ia32)"
fi

# Kbuild-generated asm-generic wrappers for both the kernel and UAPI include
# trees (see the gen_asm_wrappers comment above).  These are what make
# <asm/types.h>, <asm/io.h>, <asm/div64.h> ... resolve on x86.
gen_asm_wrappers "$SRC_DIR/include/asm-generic/Kbuild" \
    "$SRC_DIR/arch/x86/include/asm/Kbuild" \
    "$DEST/arch/x86/include/asm" \
    "$DEST/arch/x86/include/generated/asm"
gen_asm_wrappers "$SRC_DIR/include/uapi/asm-generic/Kbuild" \
    "$SRC_DIR/arch/x86/include/uapi/asm/Kbuild" \
    "$DEST/arch/x86/include/uapi/asm" \
    "$DEST/arch/x86/include/generated/uapi/asm"
log "generated asm wrappers ($(find "$DEST/arch/x86/include/generated" -name '*.h' | wc -l) header(s))"

# <generated/timeconst.h> is a Kbuild product (HZ -> conversion constants);
# derive it from the upstream bc script so <linux/jiffies.h> can be included.
if [ -f "$DEST/kernel/time/timeconst.bc" ]; then
    command -v bc >/dev/null 2>&1 || die "bc is required to generate timeconst.h"
    echo 1000 | bc -q "$DEST/kernel/time/timeconst.bc" \
        > "$DEST/include/generated/timeconst.h"
    log "generated include/generated/timeconst.h (HZ=1000)"
fi

# <lib/crc32table.h> is another Kbuild product (the slicing-by-8 tables).
# Build and run the upstream host generator; -I "$DEST/lib" resolves its
# "../include/generated/autoconf.h" include against the checked-in generated
# config linked above.
if { [ -f "$DEST/lib/crc32.c" ] || [ -f "$DEST/lib/crc/crc32-main.c" ]; } && [ -f "$SRC_DIR/lib/gen_crc32table.c" ]; then
    tmp_crc="$(mktemp -d)"
    cc -O2 -I "$DEST/lib" -I "$DEST/lib/crc" -o "$tmp_crc/gen_crc32table" \
        "$SRC_DIR/lib/gen_crc32table.c" \
        || die "failed to build gen_crc32table"
    if [ -d "$DEST/lib/crc" ]; then
        "$tmp_crc/gen_crc32table" > "$DEST/lib/crc/crc32table.h"
    else
        "$tmp_crc/gen_crc32table" > "$DEST/lib/crc32table.h"
    fi
    rm -rf "$tmp_crc"
    log "generated lib/crc32table.h"
fi

# ── Record the pin ───────────────────────────────────────────────────────────
cat > "$PIN_FILE" <<EOF
tag=$TAG
commit=$COMMIT
imported=$(date -u +%Y-%m-%dT%H:%M:%SZ)
EOF

# ── Generate the Kbuild-derived amdgpu object list (Phase 6) ─────────────────
# The amdgpu tree's Makefiles are evaluated (not hand-curated) so the object
# list, include paths and per-file flags stay in sync with the pinned tree.
# Runs after the pin is written so the generated header records the tag.
command -v python3 >/dev/null 2>&1 || die "python3 is required by kbuild-subset.py"
log "generating kernel/linux/Makefile.kbuild from the pinned Kbuild fragments"
python3 "$ROOT/scripts/linux/kbuild-subset.py" \
    --tree "$DEST" \
    --config "$DEST/include/generated/autoconf.h" \
    --out "$DEST/Makefile.kbuild" \
    --report "$DEST/kbuild-report.txt" \
    --prefix linux \
    || die "kbuild-subset.py failed"

PREVIOUS_DEST=""
if [ -e "$FINAL_DEST" ]; then
    PREVIOUS_DEST="$(mktemp -d "$ROOT/kernel/.linux-previous.XXXXXXXX")"
    rmdir "$PREVIOUS_DEST"
    mv "$FINAL_DEST" "$PREVIOUS_DEST"
fi
if ! mv "$STAGED_DEST" "$FINAL_DEST"; then
    [ -z "$PREVIOUS_DEST" ] || mv "$PREVIOUS_DEST" "$FINAL_DEST"
    die "failed to publish staged import"
fi
[ -z "$PREVIOUS_DEST" ] || rm -rf -- "$PREVIOUS_DEST"

log "done: kernel/linux @ $TAG ($COMMIT)"
log "build the kernel with: make -C kernel"
