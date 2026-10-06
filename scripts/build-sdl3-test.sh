#!/usr/bin/env bash
# scripts/build-sdl3-test.sh
# Cross-compile userland/sdl3_test.c against the Alpine edge sysroot.
# Tests SDL3 window + OpenGL context, SDL3_ttf, libplacebo, and Capstone 5.
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SYSROOT="${ROOT_DIR}/build/alpine/rootfs"

# Prefer the local musl cross-compiler built by musl-toolchain.sh
LOCAL_CC="${ROOT_DIR}/toolchain/x86_64-linux-musl/bin/x86_64-linux-musl-gcc"
if [ -x "$LOCAL_CC" ]; then
    CC="$LOCAL_CC"
elif command -v x86_64-linux-musl-gcc >/dev/null 2>&1; then
    CC="x86_64-linux-musl-gcc"
else
    echo "Error: x86_64-linux-musl-gcc not found. Run scripts/musl-toolchain.sh first." >&2
    exit 1
fi

if [ ! -d "$SYSROOT" ]; then
    echo "Error: Alpine rootfs not found at $SYSROOT" >&2
    echo "       Run scripts/setup-arch.sh first." >&2
    exit 1
fi

# Verify the critical headers exist before trying to compile
for hdr in \
    "${SYSROOT}/usr/include/SDL3/SDL.h" \
    "${SYSROOT}/usr/include/SDL3_ttf/SDL_ttf.h" \
    "${SYSROOT}/usr/include/libplacebo/log.h" \
    "${SYSROOT}/usr/include/capstone/capstone.h"; do
    if [ ! -f "$hdr" ]; then
        echo "Error: missing header $hdr" >&2
        echo "       Make sure sdl3-dev, sdl3_ttf-dev, libplacebo-dev and capstone-dev" >&2
        echo "       were installed by setup-arch.sh." >&2
        exit 1
    fi
done

INCLUDES=(
    "-I${SYSROOT}/usr/include"
    "-I${SYSROOT}/usr/include/SDL3"
    "-I${SYSROOT}/usr/include/SDL3_ttf"
)

LIBS=(
    "-L${SYSROOT}/usr/lib"
    "-L${SYSROOT}/lib"
    # SDL3 + TTF
    "-lSDL3" "-lSDL3_ttf"
    # libplacebo (EGL/OpenGL backend — no Vulkan required)
    "-lplacebo"
    # Capstone disassembly engine
    "-lcapstone"
    # OpenGL + EGL (provided by Mesa in the rootfs)
    "-lGL" "-lEGL"
    # Standard
    "-lm" "-lc"
)

LDFLAGS=(
    "-no-pie"
    "-Wl,-dynamic-linker,/lib/ld-musl-x86_64.so.1"
    "-Wl,-rpath,/usr/lib"
    "-Wl,-rpath-link,${SYSROOT}/usr/lib:${SYSROOT}/lib"
)

echo "[*] Compiling userland/sdl3_test.c ..."
if "$CC" -O2 \
    "${ROOT_DIR}/userland/sdl3_test.c" \
    -o "${ROOT_DIR}/userland/sdl3_test.elf" \
    "${INCLUDES[@]}" \
    "${LIBS[@]}" \
    "${LDFLAGS[@]}"; then
    echo "[SUCCESS] Built userland/sdl3_test.elf"
else
    echo "[FAILURE] Compilation failed" >&2
    exit 1
fi
