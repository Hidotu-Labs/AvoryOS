#!/usr/bin/env bash
# scripts/build-about.sh - Build the "About" application for AscentOS

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SYSROOT="${ROOT_DIR}/build/alpine/rootfs"
LOCAL_CC="${ROOT_DIR}/toolchain/x86_64-linux-glibc/bin/x86_64-linux-gcc"
if [ -x "$LOCAL_CC" ]; then
    CC="$LOCAL_CC"
elif [ -x "${ROOT_DIR}/toolchain/x86_64-linux-glibc/bin/x86_64-buildroot-linux-gnu-gcc" ]; then
    CC="${ROOT_DIR}/toolchain/x86_64-linux-glibc/bin/x86_64-buildroot-linux-gnu-gcc"
elif command -v x86_64-linux-gnu-gcc >/dev/null 2>&1; then
    CC="x86_64-linux-gnu-gcc"
else
    CC="gcc"
fi

if [ ! -d "$SYSROOT" ]; then
    echo "Error: Rootfs not found. Run scripts/setup-arch.sh first."
    exit 1
fi

INCLUDES=(
    "-I${SYSROOT}/usr/include/gtk-3.0"
    "-I${SYSROOT}/usr/include/glib-2.0"
    "-I${SYSROOT}/usr/lib/glib-2.0/include"
    "-I${SYSROOT}/usr/include/pango-1.0"
    "-I${SYSROOT}/usr/include/harfbuzz"
    "-I${SYSROOT}/usr/include/cairo"
    "-I${SYSROOT}/usr/include/gdk-pixbuf-2.0"
    "-I${SYSROOT}/usr/include/atk-1.0"
    "-I${SYSROOT}/usr/include/pixman-1"
    "-I${SYSROOT}/usr/include/freetype2"
    "-I${SYSROOT}/usr/include/libpng16"
    "-I${SYSROOT}/usr/include/at-spi2-atk/2.0"
    "-I${SYSROOT}/usr/include/at-spi-2.0"
    "-I${SYSROOT}/usr/include/dbus-1.0"
    "-I${SYSROOT}/usr/lib/dbus-1.0/include"
    "-I${SYSROOT}/usr/include/epoxy"
)

LIBS=(
    "-L${SYSROOT}/usr/lib"
    "-L${SYSROOT}/lib"
    "-lgtk-3" "-lgdk-3" "-lpangocairo-1.0" "-lpango-1.0" "-latk-1.0" "-latk-bridge-2.0"
    "-lcairo-gobject" "-lcairo" "-lgdk_pixbuf-2.0" "-lgio-2.0" "-lgobject-2.0" "-lglib-2.0"
    "-lepoxy" "-ldbus-1" "-lX11" "-lXext" "-lXrender" "-lXi" "-lXcursor" "-lXfixes"
    "-lwayland-client" "-lwayland-cursor" "-lwayland-egl"
    "-lXrandr" "-lXinerama" "-lXcomposite" "-lXdamage"
    "-lfontconfig" "-lfreetype" "-lpng16" "-lz" "-lm"
)

echo "[*] Compiling userland/about.c ..."
if $CC --sysroot="${SYSROOT}" -O2 \
    "${ROOT_DIR}/userland/about.c" \
    -o "${ROOT_DIR}/userland/about.elf" \
    "${INCLUDES[@]}" \
    "${LIBS[@]}" \
    -Wl,-dynamic-linker,/lib64/ld-linux-x86-64.so.2 \
    -Wl,-rpath,/usr/lib \
    -Wl,-rpath-link,${SYSROOT}/usr/lib:${SYSROOT}/lib; then
    echo "[SUCCESS] Built userland/about.elf"
else
    echo "[FAILURE] Compilation failed"
    exit 1
fi
