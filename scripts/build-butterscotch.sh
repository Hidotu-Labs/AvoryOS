#!/bin/sh
# Build Butterscotch (open-source GameMaker: Studio runner) for AvoryOS.
#
# Usage:
#   ./scripts/build-butterscotch.sh           # clone + build
#   ./scripts/build-butterscotch.sh build     # build only (skip clone if present)
#   ./scripts/build-butterscotch.sh clean     # remove build artefacts
#
# Butterscotch: https://github.com/ButterscotchRunner/Butterscotch
# The target rootfs and SDL2 package use glibc, so this target uses the matching
# glibc cross-compiler instead of the independent musl toolchain.
# The runner looks for game data (data.win / game.unx) next to the binary.
# The disk image installs both files under /opt/butterscotch/.

set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
BUILD_DIR="${BUILD_DIR:-"$ROOT_DIR/build/butterscotch"}"
SOURCE_DIR="$BUILD_DIR/Butterscotch"
OUTPUT_DIR="$ROOT_DIR/userland"
ALPINE_ROOTFS="${ALPINE_ROOTFS:-"$ROOT_DIR/build/alpine/rootfs"}"
GLIBC_TOOLCHAIN_BIN="${GLIBC_TOOLCHAIN_BIN:-"$ROOT_DIR/toolchain/x86_64-linux-glibc/bin"}"
REPO_URL="${BUTTERSCOTCH_REPO_URL:-"https://github.com/ButterscotchRunner/Butterscotch.git"}"
JOBS="${JOBS:-$(nproc 2>/dev/null || echo 4)}"

find_compiler() {
    local_cc="$GLIBC_TOOLCHAIN_BIN/x86_64-buildroot-linux-gnu-gcc"
    if [ -x "$local_cc" ]; then
        CC="$local_cc"
    elif [ -x "$GLIBC_TOOLCHAIN_BIN/x86_64-linux-gcc" ]; then
        CC="$GLIBC_TOOLCHAIN_BIN/x86_64-linux-gcc"
    elif command -v x86_64-buildroot-linux-gnu-gcc >/dev/null 2>&1; then
        CC=x86_64-buildroot-linux-gnu-gcc
    elif command -v x86_64-linux-gnu-gcc >/dev/null 2>&1; then
        CC=x86_64-linux-gnu-gcc
    else
        echo "Error: glibc cross-compiler not found; run scripts/glibc-toolchain.sh first." >&2
        exit 1
    fi
}

check_dependencies() {
    for f in \
        "$ALPINE_ROOTFS/usr/include/SDL2/SDL.h" \
        "$ALPINE_ROOTFS/usr/lib/libSDL2.so"; do
        if [ ! -e "$f" ]; then
            echo "Error: missing $f; run scripts/setup-arch.sh first." >&2
            exit 1
        fi
    done
    if ! command -v cmake >/dev/null 2>&1; then
        echo "Error: cmake not found on the host. Install cmake and retry." >&2
        exit 1
    fi
}

fetch_source() {
    mkdir -p "$BUILD_DIR"
    if [ ! -d "$SOURCE_DIR/.git" ]; then
        echo "[*] Cloning Butterscotch..."
        git clone --depth=1 "$REPO_URL" "$SOURCE_DIR"
    else
        echo "[*] Using existing Butterscotch source at $SOURCE_DIR"
    fi
}

build_butterscotch() {
    find_compiler
    check_dependencies
    fetch_source

    CMAKE_BUILD_DIR="$BUILD_DIR/cmake-build"
    mkdir -p "$CMAKE_BUILD_DIR"

    echo "[*] Configuring Butterscotch with CMake (SDL2 backend, miniaudio, glibc)..."

    # Write a minimal toolchain file for the glibc cross-compiler.
    TOOLCHAIN_FILE="$BUILD_DIR/glibc-toolchain.cmake"
    cat > "$TOOLCHAIN_FILE" <<EOF
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR x86_64)
set(CMAKE_C_COMPILER   "$CC")
set(CMAKE_SYSROOT "$ALPINE_ROOTFS")
set(CMAKE_FIND_ROOT_PATH "$ALPINE_ROOTFS")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
EOF

    # Tell pkg-config where to find Alpine's SDL2.pc
    export PKG_CONFIG_PATH="$ALPINE_ROOTFS/usr/lib/pkgconfig:$ALPINE_ROOTFS/usr/share/pkgconfig"
    export PKG_CONFIG_LIBDIR="$ALPINE_ROOTFS/usr/lib/pkgconfig"
    export PKG_CONFIG_SYSROOT_DIR="$ALPINE_ROOTFS"

    # Additional CFLAGS / LDFLAGS so the linker can resolve target shared libs
    # at link time while embedding an rpath that resolves them at runtime in the VM.
    EXTRA_CFLAGS="-I$ALPINE_ROOTFS/usr/include/SDL2 -fno-stack-protector -DMA_NO_ALSA"
    EXTRA_LDFLAGS="-L$ALPINE_ROOTFS/usr/lib -L$ALPINE_ROOTFS/lib \
-Wl,-dynamic-linker,/lib64/ld-linux-x86-64.so.2 \
-Wl,-rpath,/usr/lib \
-Wl,-rpath-link,$ALPINE_ROOTFS/usr/lib \
-Wl,-rpath-link,$ALPINE_ROOTFS/lib \
-Wl,--allow-shlib-undefined"

    # CMake caches its compiler. Drop an older musl-configured tree so the
    # glibc compiler switch cannot leave stale objects behind.
    CMAKE_COMPILER_INFO=$(find "$CMAKE_BUILD_DIR/CMakeFiles" -name CMakeCCompiler.cmake -print -quit 2>/dev/null || true)
    if [ -f "$CMAKE_BUILD_DIR/CMakeCache.txt" ] && \
       [ -n "$CMAKE_COMPILER_INFO" ] && \
       ! grep -Fq "set(CMAKE_C_COMPILER \"$CC\")" "$CMAKE_COMPILER_INFO"; then
        rm -rf "$CMAKE_BUILD_DIR"
        mkdir -p "$CMAKE_BUILD_DIR"
    fi

    cmake \
        -S "$SOURCE_DIR" \
        -B "$CMAKE_BUILD_DIR" \
        -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN_FILE" \
        -DCMAKE_BUILD_TYPE=Release \
        -DPLATFORM=cli \
        -DBACKEND=sdl2 \
        -DAUDIO_BACKEND=miniaudio \
        -DENABLE_WAD14=ON \
        -DENABLE_WAD16=ON \
        -DENABLE_WAD17=ON \
        -DENABLE_LEGACY_GL=ON \
        -DENABLE_MODERN_GL=ON \
        "-DCMAKE_C_FLAGS=$EXTRA_CFLAGS" \
        "-DCMAKE_EXE_LINKER_FLAGS=$EXTRA_LDFLAGS"

    echo "[*] Building Butterscotch (-j$JOBS)..."
    cmake --build "$CMAKE_BUILD_DIR" --parallel "$JOBS"

    # The binary is called 'butterscotch' in the build dir.
    binary=$(find "$CMAKE_BUILD_DIR" -maxdepth 2 -name butterscotch -type f | head -1)
    if [ -z "$binary" ]; then
        echo "Error: Butterscotch binary not found after build." >&2
        exit 1
    fi

    cp "$binary" "$OUTPUT_DIR/butterscotch.elf"
    echo "[+] Butterscotch installed to $OUTPUT_DIR/butterscotch.elf"
}

do_clean() {
    rm -rf "$BUILD_DIR"
    rm -f "$OUTPUT_DIR/butterscotch.elf"
    echo "[+] Cleaned Butterscotch build artefacts."
}

case "${1:-build}" in
    build|"") build_butterscotch ;;
    clean)    do_clean ;;
    *) echo "Usage: $0 [build|clean]" >&2; exit 2 ;;
esac
