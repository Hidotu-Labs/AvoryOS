#!/bin/sh
# AscentOS: Build Tiny C Compiler (TCC) for AscentOS using GLIBC
# 
# This script cross-compiles TCC using the glibc toolchain, producing a
# dynamically-linked tcc binary that runs on AscentOS.

set -e

TCC_VERSION=${TCC_VERSION:-0.9.27}
TCC_TARBALL="tcc-${TCC_VERSION}.tar.bz2"
TCC_URL="https://download.savannah.gnu.org/releases/tinycc/${TCC_TARBALL}"

ROOT_DIR=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
BUILD_DIR=${BUILD_DIR:-"$ROOT_DIR/build/tcc-glibc-${TCC_VERSION}"}
PREFIX="$ROOT_DIR/toolchain/glibc-sysroot"
TCC_INSTALL="$ROOT_DIR/build/tcc-glibc-install"
JOBS=$(nproc 2>/dev/null || echo 4)

find_compiler() {
    CC="$ROOT_DIR/toolchain/x86_64-linux-glibc/bin/x86_64-linux-gcc"
    
    if [ ! -x "$CC" ]; then
        echo "Error: glibc compiler not found at $CC" >&2
        exit 1
    fi
    
    echo "Using CC=$CC"
}

do_clean() {
    echo "Cleaning $BUILD_DIR ..."
    rm -rf "$BUILD_DIR"
    rm -rf "$TCC_INSTALL"
    echo "Done."
}

build_tcc() {
    find_compiler
    
    mkdir -p "$BUILD_DIR"
    cd "$BUILD_DIR"
    
    if [ ! -f "$TCC_TARBALL" ]; then
        echo "Downloading $TCC_URL ..."
        curl -L -o "$TCC_TARBALL" "$TCC_URL"
    fi
    
    if [ ! -d "tcc-${TCC_VERSION}" ]; then
        tar xf "$TCC_TARBALL"
    fi
    
    cd "tcc-${TCC_VERSION}"
    
    # Patch tcc.c to show AvoryOS instead of Linux in the version string
    sed -i 's/" Linux"/" AvoryOS"/g' tcc.c
    
    # Configure for AscentOS (glibc target)
    echo "Configuring TCC for AscentOS (GLIBC) ..."
    
    AR="${CC%gcc}ar"
    RANLIB="${CC%gcc}ranlib"
    STRIP="${CC%gcc}strip"
    
    export CC AR RANLIB
    
    # Runtime paths on AscentOS
    RUNTIME_TCCDIR="/opt/tcc/lib/tcc"
    # We want TCC to find glibc in standard paths and its own internal headers
    SYS_INCLUDE_PATHS="/opt/tcc/lib/tcc/include:/usr/include"
    SYS_LIB_PATHS="/lib64:/usr/lib64:/usr/lib"
    # Use the glibc dynamic linker path
    DYNAMIC_LINKER="/lib64/ld-linux-x86-64.so.2"

    # Clear build directory to ensure a fresh start
    rm -f config.mak config.h
    make clean || true
    rm -f *.o libtcc.so tcc

    ./configure \
        --prefix="/opt/tcc" \
        --cc="$CC" \
        --disable-static \
        --disable-rpath \
        --tccdir="$RUNTIME_TCCDIR" \
        --sysincludepaths="$SYS_INCLUDE_PATHS" \
        --libpaths="$SYS_LIB_PATHS" \
        --elfinterp="$DYNAMIC_LINKER" \
        --extra-cflags="-O2 -Wall -fno-stack-protector -fPIC --sysroot=$PREFIX" \
        --extra-ldflags="--sysroot=$PREFIX"
    
    echo "Building TCC ..."
    make -j"$JOBS" x86_64-libtcc1-usegcc=yes
    
    echo "Installing TCC to $TCC_INSTALL ..."
    make install DESTDIR="$TCC_INSTALL"
    
    # Strip the binary
    if [ -x "$STRIP" ]; then
        echo "Stripping tcc binary ..."
        "$STRIP" "$TCC_INSTALL/opt/tcc/bin/tcc"
    fi
    
    echo "Verifying TCC installation..."
    ls -l "$TCC_INSTALL/opt/tcc/bin/tcc"
    
    cd "$ROOT_DIR"
    
    echo ""
    echo "========================================"
    echo "TCC ${TCC_VERSION} (GLIBC) built successfully!"
    echo "========================================"
}

case "${1:-build}" in
    clean)
        do_clean
        ;;
    build|"")
        build_tcc
        ;;
    *)
        echo "Usage: $0 [build|clean]" >&2
        exit 1
        ;;
esac
