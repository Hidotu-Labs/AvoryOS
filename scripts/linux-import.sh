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
#   scripts/linux-import.sh                 # use pin, or resolve latest v6.6.*
#   LINUX_VERSION=v6.6.62 scripts/linux-import.sh
#   scripts/linux-import.sh --force         # re-copy even if the pin matches
#
# Environment:
#   LINUX_VERSION     tag to import (default: auto; latest v6.6.* stable)
#   LINUX_REPO        git remote (default: kernel.org stable tree)
#   LINUX_SRC_DIR     clone cache (default: build/linux-src)
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
LINUX_REPO="${LINUX_REPO:-https://git.kernel.org/pub/scm/linux/kernel/git/stable/linux.git}"
LINUX_VERSION="${LINUX_VERSION:-auto}"
SRC_DIR="${LINUX_SRC_DIR:-$ROOT/build/linux-src}"
DEST="$ROOT/kernel/linux"
SUBSET_FILE="$ROOT/scripts/linux/subset.txt"
FILES_FILE="$ROOT/scripts/linux/files.txt"
GEN_SRC="$ROOT/kernel/linuxkpi/include/generated"
FORCE=0

for arg in "$@"; do
    case "$arg" in
        --force) FORCE=1 ;;
        -h|--help) sed -n '2,25p' "$0"; exit 0 ;;
        *) echo "linux-import: unknown argument: $arg" >&2; exit 2 ;;
    esac
done

log()  { printf '[linux-import] %s\n' "$*"; }
die()  { printf '[linux-import] error: %s\n' "$*" >&2; exit 1; }

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
log "commit: $COMMIT"

# ── Copy the subset ──────────────────────────────────────────────────────────
log "copying subset into kernel/linux/"
rm -rf "$DEST"
mkdir -p "$DEST"
while IFS= read -r path; do
    case "$path" in ''|\#*) continue ;; esac
    # Entries may contain shell globs (e.g. drivers/gpu/drm/*.h): copy every
    # match.  A literal path that does not exist is an error.
    matched=0
    for src in "$SRC_DIR"/$path; do
        [ -e "$src" ] || continue
        matched=1
        rel="${src#"$SRC_DIR"/}"
        mkdir -p "$DEST/$(dirname "$rel")"
        cp -a "$src" "$DEST/$rel"
    done
    [ "$matched" -eq 1 ] || die "subset path missing in tree: $path"
done < "$SUBSET_FILE"

# ── Generate Makefile.files from scripts/linux/files.txt ─────────────────────
mapfile -t srcfiles < <(grep -v '^[[:space:]]*\(#\|$\)' "$FILES_FILE")
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
if [ -f "$DEST/lib/crc32.c" ] && [ -f "$SRC_DIR/lib/gen_crc32table.c" ]; then
    tmp_crc="$(mktemp -d)"
    cc -O2 -I "$DEST/lib" -o "$tmp_crc/gen_crc32table" \
        "$SRC_DIR/lib/gen_crc32table.c" \
        || die "failed to build gen_crc32table"
    "$tmp_crc/gen_crc32table" > "$DEST/lib/crc32table.h"
    rm -rf "$tmp_crc"
    log "generated lib/crc32table.h"
fi

# ── Record the pin ───────────────────────────────────────────────────────────
cat > "$PIN_FILE" <<EOF
tag=$TAG
commit=$COMMIT
imported=$(date -u +%Y-%m-%dT%H:%M:%SZ)
EOF

log "done: kernel/linux @ $TAG ($COMMIT)"
log "build the kernel with: make -C kernel"
