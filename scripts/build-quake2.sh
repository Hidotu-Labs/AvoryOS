#!/bin/sh
# Build Yamagi Quake II and optionally fetch id Software's Quake II demo data.
#
# Usage:
#   ./scripts/build-quake2.sh             # build engine + fetch demo data
#   ./scripts/build-quake2.sh build       # build engine only
#   ./scripts/build-quake2.sh data        # fetch/extract demo data only
#   ./scripts/build-quake2.sh clean

set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
BUILD_DIR=${BUILD_DIR:-"$ROOT_DIR/build/quake2"}
SOURCE_DIR="$BUILD_DIR/yquake2"
OUTPUT_DIR="$ROOT_DIR/userland/quake2"
ALPINE_ROOTFS=${ALPINE_ROOTFS:-"$ROOT_DIR/build/alpine/rootfs"}
REPO_URL=${YQUAKE2_REPO_URL:-"https://github.com/yquake2/yquake2.git"}
DEMO_URL=${QUAKE2_DEMO_URL:-"https://ftp.gwdg.de/pub/misc/ftp.idsoftware.com/idstuff/quake2/q2-314-demo-x86.exe"}
DEMO_SHA256=7ace5a43983f10d6bdc9d9b6e17a1032ba6223118d389bd170df89b945a04a1e
JOBS=${JOBS:-$(nproc 2>/dev/null || echo 4)}

find_compiler() {
    local_cc="$ROOT_DIR/toolchain/x86_64-linux-glibc/bin/x86_64-buildroot-linux-gnu-gcc"
    if [ -x "$local_cc" ]; then
        CC="$local_cc"
    elif [ -x "$ROOT_DIR/toolchain/x86_64-linux-glibc/bin/x86_64-linux-gcc" ]; then
        CC="$ROOT_DIR/toolchain/x86_64-linux-glibc/bin/x86_64-linux-gcc"
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
    for file in \
        "$ALPINE_ROOTFS/usr/include/SDL2/SDL.h" \
        "$ALPINE_ROOTFS/usr/lib/libSDL2.so" \
        "$ALPINE_ROOTFS/usr/lib/libz.so"; do
        if [ ! -e "$file" ]; then
            echo "Error: missing $file; run scripts/setup-arch.sh first." >&2
            exit 1
        fi
    done
}

fetch_source() {
    mkdir -p "$BUILD_DIR"
    if [ ! -d "$SOURCE_DIR/.git" ]; then
        echo "[*] Cloning Yamagi Quake II..."
        git clone --depth=1 "$REPO_URL" "$SOURCE_DIR"
    else
        echo "[*] Using existing source at $SOURCE_DIR"
    fi
}

patch_source() {
    main_c="$SOURCE_DIR/src/backends/unix/main.c"
    input_c="$SOURCE_DIR/src/client/input/sdl2.c"
    if grep -q '^#ifndef __HAIKU__$' "$main_c"; then
        sed -i 's/^#ifndef __HAIKU__$/#if !defined(__HAIKU__) \&\& !defined(ASCENTOS)/' "$main_c"
    fi
    if ! grep -q '^#if !defined(__HAIKU__) && !defined(ASCENTOS)$' "$main_c"; then
        echo "Error: could not apply the AscentOS root-session patch to $main_c" >&2
        exit 1
    fi
    cp "$ROOT_DIR/scripts/quake2-avoryos-evdev.h" "$SOURCE_DIR/src/client/input/ascentos_evdev.h"
    if ! grep -q 'ASCENTOS_EVDEV_KEYBOARD' "$input_c"; then
        sed -i '/#include "\.\.\/header\/client.h"/a\
\
#ifdef ASCENTOS\
#define ASCENTOS_EVDEV_KEYBOARD 1\
#include "ascentos_evdev.h"\
#endif' "$input_c"
        sed -i '/IN_Update(void)/,/^{/{/^{/a\
#ifdef ASCENTOS_EVDEV_KEYBOARD\
\tAscentOS_KeyboardUpdate();\
#endif
}' "$input_c"
        sed -i '/SDL_StartTextInput();/a\
\
#ifdef ASCENTOS_EVDEV_KEYBOARD\
\tAscentOS_KeyboardInit();\
#endif' "$input_c"
        sed -i '/Com_Printf("Shutting down input\\.\\n");/a\
\
#ifdef ASCENTOS_EVDEV_KEYBOARD\
\tAscentOS_KeyboardShutdown();\
#endif' "$input_c"
    fi
    if ! grep -q 'AscentOS_KeyboardShutdown();' "$input_c"; then
        sed -i '/IN_Shutdown(void)/,/IN_Controller_Shutdown(false);/{/IN_Controller_Shutdown(false);/i\
#ifdef ASCENTOS_EVDEV_KEYBOARD\
\tAscentOS_KeyboardShutdown();\
#endif\

}' "$input_c"
    fi
    grep -q 'AscentOS_KeyboardUpdate();' "$input_c" || {
        echo "Error: could not apply AscentOS keyboard patch" >&2
        exit 1
    }
}

build_engine() {
    find_compiler
    check_dependencies
    fetch_source
    patch_source

    wrapper_dir="$BUILD_DIR/tools"
    mkdir -p "$wrapper_dir"
    sed \
        -e "s|@INCLUDE@|$ALPINE_ROOTFS/usr/include/SDL2|g" \
        -e "s|@LIB@|$ALPINE_ROOTFS/usr/lib|g" \
        "$ROOT_DIR/scripts/quake2-sdl2-config.in" > "$wrapper_dir/sdl2-config"
    chmod +x "$wrapper_dir/sdl2-config"

    # The source tree's generated .d files can retain headers from the former
    # musl/Alpine build (for example bits/alltypes.h), which do not exist in
    # the current glibc rootfs. Drop those objects before switching libc.
    if [ -f "$SOURCE_DIR/config.mk" ] && \
       ! grep -Fqx "CC=$CC" "$SOURCE_DIR/config.mk"; then
        make -C "$SOURCE_DIR" clean
    fi

    cat > "$SOURCE_DIR/config.mk" <<EOF
CC=$CC
YQ2_OSTYPE=Linux
YQ2_ARCH=x86_64
WITH_CURL=no
WITH_EXECINFO=no
WITH_OPENAL=no
WITH_RPATH=no
WITH_SDL3=no
WITH_SYSTEMWIDE=no
WITH_XDG=no
INCLUDE=--sysroot=$ALPINE_ROOTFS -I$ALPINE_ROOTFS/usr/include
CFLAGS=--sysroot=$ALPINE_ROOTFS -O2 -Wall -pipe -fomit-frame-pointer -fno-stack-protector -DASCENTOS
LDFLAGS=--sysroot=$ALPINE_ROOTFS -L$ALPINE_ROOTFS/usr/lib -L$ALPINE_ROOTFS/lib -Wl,-dynamic-linker,/lib64/ld-linux-x86-64.so.2 -Wl,-rpath-link,$ALPINE_ROOTFS/usr/lib -Wl,-rpath-link,$ALPINE_ROOTFS/lib -Wl,--allow-shlib-undefined -Wl,-rpath,/usr/lib:/lib
EOF

    echo "[*] Building Yamagi Quake II (SDL2 software renderer)..."
    unset DEBUG
    PATH="$wrapper_dir:$PATH" make -C "$SOURCE_DIR" -j"$JOBS" client game ref_soft

    mkdir -p "$OUTPUT_DIR/baseq2"
    cp "$SOURCE_DIR/release/quake2" "$OUTPUT_DIR/quake2"
    cp "$SOURCE_DIR/release/ref_soft.so" "$OUTPUT_DIR/ref_soft.so"
    cp "$SOURCE_DIR/release/baseq2/game.so" "$OUTPUT_DIR/baseq2/game.so"
    if [ -f "$SOURCE_DIR/stuff/yq2.cfg" ]; then
        cp "$SOURCE_DIR/stuff/yq2.cfg" "$OUTPUT_DIR/baseq2/yq2.cfg"
    fi
    cat > "$OUTPUT_DIR/baseq2/autoexec.cfg" <<'EOF'
set vid_renderer "soft"
set vid_fullscreen "0"
set r_mode "4"
set r_vsync "0"
set s_initsound "1"
bind w "+forward"
bind s "+back"
bind a "+moveleft"
bind d "+moveright"
bind SPACE "+moveup"
bind CTRL "+movedown"
EOF
    chmod +x "$OUTPUT_DIR/quake2"
    echo "[+] Engine installed in $OUTPUT_DIR"
}

fetch_demo_data() {
    command -v curl >/dev/null 2>&1 || {
        echo "Error: curl is required to fetch the Quake II demo." >&2
        exit 1
    }
    command -v unzip >/dev/null 2>&1 || {
        echo "Error: unzip is required to extract the Quake II demo." >&2
        exit 1
    }

    mkdir -p "$BUILD_DIR/demo" "$OUTPUT_DIR/baseq2"
    demo="$BUILD_DIR/demo/q2-314-demo-x86.exe"
    if [ ! -f "$demo" ]; then
        echo "[*] Downloading the official id Software Quake II demo (about 39 MB)..."
        curl -fL --retry 3 -o "$demo.part" "$DEMO_URL"
        mv "$demo.part" "$demo"
    fi
    if command -v sha256sum >/dev/null 2>&1; then
        actual_sha256=$(sha256sum "$demo" | awk '{print $1}')
        if [ "$actual_sha256" != "$DEMO_SHA256" ]; then
            echo "Error: Quake II demo archive checksum mismatch." >&2
            rm -f "$demo"
            exit 1
        fi
    fi
    if [ ! -f "$BUILD_DIR/demo/Install/Data/baseq2/pak0.pak" ]; then
        echo "[*] Extracting demo data..."
        (cd "$BUILD_DIR/demo" && unzip -q -o "$demo")
    fi
    cp "$BUILD_DIR/demo/Install/Data/baseq2/pak0.pak" "$OUTPUT_DIR/baseq2/pak0.pak"
    echo "[+] Demo game data installed in $OUTPUT_DIR/baseq2"
    echo "    For the full game, copy legally owned pak*.pak files into that directory."
}

do_clean() {
    rm -rf "$BUILD_DIR" "$OUTPUT_DIR"
    echo "[+] Removed Quake II build and output files."
}

case "${1:-all}" in
    all) build_engine; fetch_demo_data ;;
    build) build_engine ;;
    data) fetch_demo_data ;;
    clean) do_clean ;;
    *) echo "Usage: $0 [all|build|data|clean]" >&2; exit 2 ;;
esac
