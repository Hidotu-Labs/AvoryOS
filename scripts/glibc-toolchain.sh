#!/bin/sh
# Download the prebuilt x86_64 GCC cross-compiler used to build AvoryOS
# userland. The libc sysroot comes from the Axon rootfs, not this toolchain.

set -eu

TOOLCHAIN_VERSION="2024.02-1"
TOOLCHAIN_TARBALL="x86-64--glibc--stable-${TOOLCHAIN_VERSION}.tar.bz2"
TOOLCHAIN_URL="https://toolchains.bootlin.com/downloads/releases/toolchains/x86-64/tarballs/${TOOLCHAIN_TARBALL}"

ROOT_DIR=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
OUTPUT_DIR="$ROOT_DIR/toolchain/x86_64-linux-glibc"
BUILD_DIR="$ROOT_DIR/build/glibc-toolchain"
TARGET_CC="$OUTPUT_DIR/bin/x86_64-buildroot-linux-gnu-gcc"

mkdir -p "$BUILD_DIR" "$ROOT_DIR/toolchain"

if [ ! -x "$TARGET_CC" ]; then
    echo "--- Downloading prebuilt GCC cross-compiler from Bootlin ---"
    if [ ! -f "$BUILD_DIR/$TOOLCHAIN_TARBALL" ]; then
        curl -fL "$TOOLCHAIN_URL" -o "$BUILD_DIR/$TOOLCHAIN_TARBALL"
    fi

    echo "--- Extracting prebuilt GCC cross-compiler ---"
    mkdir -p "$OUTPUT_DIR"
    tar -xjf "$BUILD_DIR/$TOOLCHAIN_TARBALL" -C "$OUTPUT_DIR" --strip-components=1
else
    echo "Prebuilt GCC cross-compiler already exists at $OUTPUT_DIR."
fi

if [ ! -x "$TARGET_CC" ]; then
    echo "[!] Cross-compiler was not installed at $TARGET_CC." >&2
    exit 1
fi

# The compiler's bundled fixincludes were generated for its old sysroot.
# Always use the Axon rootfs pthread header with the target glibc 2.44 ABI.
rm -f "$OUTPUT_DIR"/lib/gcc/*/*/include-fixed/pthread.h

# Preserve the compiler aliases used by existing build scripts.
for tool in gcc g++ cpp ar as ld nm objcopy objdump ranlib strip; do
    if [ -f "$OUTPUT_DIR/bin/x86_64-buildroot-linux-gnu-$tool" ]; then
        ln -sfn "x86_64-buildroot-linux-gnu-$tool" "$OUTPUT_DIR/bin/x86_64-linux-$tool"
        ln -sfn "x86_64-buildroot-linux-gnu-$tool" "$OUTPUT_DIR/bin/x86_64-linux-gnu-$tool"
    fi
done

echo "GCC cross-compiler ready. Build userland with the Axon rootfs as --sysroot."
