#!/bin/sh
# AscentOS: download and setup x86_64 glibc toolchain from Bootlin
#
# This script avoids the long build time of glibc by downloading a 
# pre-built, production-ready toolchain.

set -e

TOOLCHAIN_VERSION="2024.02-1"
TOOLCHAIN_TARBALL="x86-64--glibc--stable-${TOOLCHAIN_VERSION}.tar.bz2"
TOOLCHAIN_URL="https://toolchains.bootlin.com/downloads/releases/toolchains/x86-64/tarballs/${TOOLCHAIN_TARBALL}"

GLIBC_VERSION="2.43"
GLIBC_TARBALL="glibc-${GLIBC_VERSION}.tar.xz"
GLIBC_URL="https://ftp.gnu.org/gnu/glibc/${GLIBC_TARBALL}"

ROOT_DIR=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
OUTPUT_DIR="$ROOT_DIR/toolchain/x86_64-linux-glibc"
BUILD_DIR="$ROOT_DIR/build/glibc-toolchain"
SYSROOT="$OUTPUT_DIR/x86_64-buildroot-linux-gnu/sysroot"

mkdir -p "$BUILD_DIR"
mkdir -p "$ROOT_DIR/toolchain"

# 1. Download base cross-toolchain from Bootlin if not present
if [ ! -d "$OUTPUT_DIR" ] || [ ! -f "$OUTPUT_DIR/bin/x86_64-buildroot-linux-gnu-gcc" ]; then
    echo "--- Downloading base glibc toolchain from Bootlin ---"
    if [ ! -f "$BUILD_DIR/$TOOLCHAIN_TARBALL" ]; then
        curl -L "$TOOLCHAIN_URL" -o "$BUILD_DIR/$TOOLCHAIN_TARBALL"
    fi

    echo "--- Extracting base glibc toolchain ---"
    mkdir -p "$OUTPUT_DIR"
    tar -xjf "$BUILD_DIR/$TOOLCHAIN_TARBALL" -C "$OUTPUT_DIR" --strip-components=1
    echo "Base glibc toolchain installed to: $OUTPUT_DIR"
else
    echo "Base glibc toolchain already exists at $OUTPUT_DIR."
fi

# 2. Upgrade glibc to 2.42 in the sysroot
GLIBC_SRC_DIR="$BUILD_DIR/glibc-${GLIBC_VERSION}"
GLIBC_BUILD_DIR="$BUILD_DIR/glibc-build-${GLIBC_VERSION}"
MARKER_FILE="$OUTPUT_DIR/.glibc-${GLIBC_VERSION}-installed"

if [ ! -f "$MARKER_FILE" ]; then
    echo "--- Upgrading glibc to version ${GLIBC_VERSION} ---"
    if [ ! -f "$BUILD_DIR/$GLIBC_TARBALL" ]; then
        echo "Downloading glibc ${GLIBC_VERSION}..."
        curl -L "$GLIBC_URL" -o "$BUILD_DIR/$GLIBC_TARBALL"
    fi

    if [ ! -d "$GLIBC_SRC_DIR" ]; then
        echo "Extracting glibc ${GLIBC_VERSION}..."
        tar -xf "$BUILD_DIR/$GLIBC_TARBALL" -C "$BUILD_DIR"
    fi

    # Upstream glibc 2.43 fix: define __glibc_has_open_how to prevent duplicate struct open_how
    sed -i '/#ifndef __glibc_has_open_how/a #define __glibc_has_open_how 1' "$GLIBC_SRC_DIR/sysdeps/unix/sysv/linux/bits/openat2.h" 2>/dev/null || true
    sed -i '/#include <bits\/openat2.h>/d' "$GLIBC_SRC_DIR/sysdeps/unix/sysv/linux/openat2.c" 2>/dev/null || true

    mkdir -p "$GLIBC_BUILD_DIR"
    cd "$GLIBC_BUILD_DIR"

    TARGET_CC="$OUTPUT_DIR/bin/x86_64-buildroot-linux-gnu-gcc"
    if [ ! -x "$TARGET_CC" ]; then
        TARGET_CC="gcc"
    fi

    echo "Configuring glibc ${GLIBC_VERSION}..."
    CC="$TARGET_CC" CXX="$OUTPUT_DIR/bin/x86_64-buildroot-linux-gnu-g++" \
    CFLAGS="-O2 -pipe" \
    "$GLIBC_SRC_DIR/configure" \
        --prefix=/usr \
        --host=x86_64-buildroot-linux-gnu \
        --build=$(gcc -dumpmachine 2>/dev/null || echo "x86_64-linux-gnu") \
        --with-headers="$SYSROOT/usr/include" \
        --enable-kernel=4.4.0 \
        --disable-werror \
        --disable-profile \
        --enable-shared \
        --enable-static

    echo "Compiling glibc ${GLIBC_VERSION}..."
    make -j$(nproc 2>/dev/null || echo 4)

    echo "Installing glibc ${GLIBC_VERSION} into sysroot..."
    make DESTDIR="$SYSROOT" install

    # Fixup library paths: ensure /lib and /usr/lib are populated
    if [ -d "$SYSROOT/usr/lib64" ] && [ ! -d "$SYSROOT/usr/lib" ]; then
        ln -sfn lib64 "$SYSROOT/usr/lib"
    fi
    if [ -d "$SYSROOT/usr/lib" ]; then
        mkdir -p "$SYSROOT/lib"
        for f in "$SYSROOT"/usr/lib/libc.so* "$SYSROOT"/usr/lib/libm.so* "$SYSROOT"/usr/lib/ld-linux*.so*; do
            if [ -f "$f" ]; then
                cp -a "$f" "$SYSROOT/lib/" 2>/dev/null || true
            fi
        done
    fi

    touch "$MARKER_FILE"
    echo "glibc ${GLIBC_VERSION} successfully upgraded and installed into sysroot."
else
    echo "glibc ${GLIBC_VERSION} already installed. Skipping build."
fi

# The base Bootlin toolchain ships an include-fixed/pthread.h fixincluded
# against its original (older) glibc.  After the sysroot upgrade above that
# shadow header is stale: C++ library headers reach it through
# <bits/gthr-default.h>, and its old PTHREAD_COND_INITIALIZER no longer
# matches the sysroot's glibc 2.43 pthread_cond_t layout (breaks e.g.
# userland/qt5_test.elf with "__gthread_cond_t _M_cond = ..." conversion
# errors).  Remove it so <pthread.h> resolves to the sysroot's real header.
rm -f "$OUTPUT_DIR"/lib/gcc/*/*/include-fixed/pthread.h

# Create convenience aliases
if [ -d "$OUTPUT_DIR/bin" ]; then
    for tool in gcc g++ cpp ar as ld nm objcopy objdump ranlib strip; do
        if [ -f "$OUTPUT_DIR/bin/x86_64-buildroot-linux-gnu-$tool" ]; then
            ln -sfn "x86_64-buildroot-linux-gnu-$tool" "$OUTPUT_DIR/bin/x86_64-linux-$tool"
            ln -sfn "x86_64-buildroot-linux-gnu-$tool" "$OUTPUT_DIR/bin/x86_64-linux-gnu-$tool"
        fi
    done
fi

# Create a symlink to the sysroot if needed
SYSROOT="$OUTPUT_DIR/x86_64-buildroot-linux-gnu/sysroot"
if [ -d "$SYSROOT" ]; then
    ln -sfn "$SYSROOT" "$ROOT_DIR/toolchain/glibc-sysroot"
    echo "glibc sysroot linked to: $ROOT_DIR/toolchain/glibc-sysroot"
fi

echo "All glibc toolchain dependencies have been set up."
