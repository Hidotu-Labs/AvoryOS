#!/usr/bin/env bash
# scripts/setup-minecraft.sh -- Stage Minecraft 1.16.5 into AvoryOS
#
# Prerequisites:
#   1. Run ./scripts/setup-rootfs.sh first (provides the AvoryOS rootfs and disk.img).
#      Axon installs glibc, OpenJDK 17, OpenAL, Mesa and the X11 client libraries
#      into that rootfs when needed.
#   2. Place the Minecraft 1.16.5 files in assets/minecraft/ before running.
#      This script stages local files only and does not fetch Minecraft content.
#
# Usage:
#   ./scripts/setup-minecraft.sh [--expand-disk] [--username NAME] [--jar PATH]
#
# Options:
#   --expand-disk   Grow disk.img to 8 GiB
#   --username NAME Offline username written into the launcher (default: Player)
#   --jar PATH      Use a locally saved 1.16.5 client JAR instead of
#                   assets/minecraft/client.jar

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DISK_IMG="${ROOT_DIR}/disk.img"
BUILD_DIR="${ROOT_DIR}/build/arch"
ROOTFS_DIR="${BUILD_DIR}/rootfs"
POPULATE_SCRIPT="${ROOT_DIR}/scripts/populate-ext2-dir.sh"
source "${ROOT_DIR}/scripts/axon-common.sh"

MC_VERSION="1.16.5"
MC_CLIENT_SHA1="37fd3c903861eeff3bc24b71eed48f828b5269c8"
MC_ASSET_INDEX="1.16"
MC_LOCAL_DIR="${ROOT_DIR}/assets/minecraft"

MC_JAR_OVERRIDE=""
EXPAND_DISK=0
MC_USERNAME="Player"
while [[ $# -gt 0 ]]; do
    case "$1" in
        --expand-disk) EXPAND_DISK=1; shift ;;
        --username)    MC_USERNAME="${2:-Player}"; shift 2 ;;
        --jar)         MC_JAR_OVERRIDE="${2:-}"; shift 2 ;;
        *) echo "[!] Unknown argument: $1" >&2; exit 1 ;;
    esac
done

# ─────────────────────────────────────────────────────────────────────────────
# Sanity checks
# ─────────────────────────────────────────────────────────────────────────────
[ ! -d "${ROOTFS_DIR}/etc" ] && {
    echo "[!] AvoryOS rootfs not found. Run ./scripts/setup-rootfs.sh first."
    exit 1
}
[ ! -f "${DISK_IMG}" ] && {
    echo "[!] disk.img not found. Run 'make disk.img' first."
    exit 1
}

# ─────────────────────────────────────────────────────────────────────────────
# Ensure glibc + libraries from the AvoryOS rootfs (install if missing)
# ─────────────────────────────────────────────────────────────────────────────
ensure_axon

# Repair glibc version skew if present: an older setup redirected the system
# loader at the 2.43 copy in /opt/avory-glibc while /usr/lib ships 2.44.
# Mixing the two breaks every dlopen (undefined symbol: __pointer_chk_guard),
# which is exactly how Minecraft's libglfw.so fails to load.
for _glibc_link in "${ROOTFS_DIR}/usr/lib/libc.so.6" "${ROOTFS_DIR}/usr/lib/ld-linux-x86-64.so.2"; do
    if [ -L "${_glibc_link}" ]; then
        echo "[!] ${_glibc_link} is a stray symlink. Rebuild the rootfs with 'make setup-rootfs' before staging Minecraft." >&2
        exit 1
    fi
done

# glibc itself plus everything Minecraft 1.16.5 (LWJGL 3/GLFW via OpenJDK 17)
# loads at runtime. Each is installed only if missing from the rootfs.
# Minecraft 1.16.5 uses LWJGL 3/GLFW, which dlopen()s these X11 client
# libraries at window-creation time.
"${AXON_BIN}" get --root "${ROOTFS_DIR}" --needed --noconfirm --keep-cache \
    --assume-installed ntsync-autoload \
    glibc jre17-openjdk-headless jre17-openjdk openal mesa \
    libx11 libxext libxi libxrandr libxcursor libxinerama libxxf86vm

find_java() {
    JAVA_HOME_ROOTFS=""
    for cand in \
        "${ROOTFS_DIR}/usr/lib/jvm/java-17-openjdk" \
        "${ROOTFS_DIR}/usr/lib/jvm/default-runtime"; do
        if [ -d "${cand}" ]; then
            JAVA_HOME_ROOTFS="${cand}"
            return 0
        fi
    done
    if [ -z "${JAVA_HOME_ROOTFS}" ]; then
        JAVA_HOME_ROOTFS=$(ls -d "${ROOTFS_DIR}/usr/lib/jvm"/java-17* 2>/dev/null | head -1 || true)
    fi
    return 0
}

find_java

[ -z "${JAVA_HOME_ROOTFS}" ] && {
    echo "[!] Failed to find OpenJDK 17 in the AvoryOS rootfs. Aborting." >&2
    exit 1
}

JAVA_HOME_GUEST="${JAVA_HOME_ROOTFS#"${ROOTFS_DIR}"}"
echo "[+] Using JDK from AvoryOS rootfs: ${JAVA_HOME_GUEST}"
# NOTE: no musl-era fixups here. The Axon-provisioned tree is glibc, so the JVM's own
# RPATH/$ORIGIN layout, Mojang's glibc-built LWJGL natives and the bundled
# OpenAL all resolve natively — no .so copying into /usr/lib, no compat shim.

# ─────────────────────────────────────────────────────────────────────────────
# 1. Stage locally supplied Minecraft libraries + Linux natives
# ─────────────────────────────────────────────────────────────────────────────
echo ""
echo "=== [1/4] Staging local Minecraft ${MC_VERSION} libraries ==="

LIBS_DIR="${BUILD_DIR}/mc1165-libs"
NATIVES_BUILD_DIR="${BUILD_DIR}/mc1165-natives"
NATIVES_LIST="${BUILD_DIR}/mc1165-natives.list"
MC_LIBS_ROOTFS="${ROOTFS_DIR}/opt/minecraft/libs"
MC_NATIVES_ROOTFS="${ROOTFS_DIR}/opt/minecraft/natives"
mkdir -p "${LIBS_DIR}" "${NATIVES_BUILD_DIR}" "${MC_LIBS_ROOTFS}" "${MC_NATIVES_ROOTFS}"

# Remove anything staged by an older setup run: leftover LWJGL 2 and
# log4j 2.0-beta9 JARs from 1.8.9 must not end up on the 1.16.5 classpath.
rm -f "${MC_LIBS_ROOTFS:?}"/*.jar
rm -f "${MC_NATIVES_ROOTFS:?}"/*.so

LOCAL_LIBS_DIR="${MC_LOCAL_DIR}/libs"
LOCAL_NATIVES_DIR="${MC_LOCAL_DIR}/natives"
if [ ! -d "${LOCAL_LIBS_DIR}" ] || [ ! -d "${LOCAL_NATIVES_DIR}" ]; then
    echo "[!] Local libraries/natives missing. Expected ${LOCAL_LIBS_DIR} and ${LOCAL_NATIVES_DIR}." >&2
    exit 1
fi
find "${LOCAL_LIBS_DIR}" -maxdepth 1 -type f -name '*.jar' -exec cp -f {} "${LIBS_DIR}/" \;
find "${LOCAL_NATIVES_DIR}" -maxdepth 1 -type f -name '*.jar' -exec cp -f {} "${NATIVES_BUILD_DIR}/" \;
find "${NATIVES_BUILD_DIR}" -maxdepth 1 -type f -name '*.jar' -print | sort > "${NATIVES_LIST}"
if [ ! -s "${LIBS_DIR}"/*.jar ] 2>/dev/null; then
    echo "[!] No library JARs found in ${LOCAL_LIBS_DIR}." >&2
    exit 1
fi

# ── Extract the Linux natives (LWJGL 3.2.2, GLFW, OpenAL, STB, ...) ─────────
echo "[*] Extracting Linux native .so files..."
while IFS= read -r nat_jar; do
    [ -n "${nat_jar}" ] || continue
    if [ ! -f "${nat_jar}" ]; then
        echo "[!] Missing native JAR: ${nat_jar}" >&2
        exit 1
    fi
    unzip -oj "${nat_jar}" "*.so" -d "${MC_NATIVES_ROOTFS}" 2>/dev/null || true
done < "${NATIVES_LIST}"

# LWJGL bundles its own OpenAL; prefer the Arch rootfs glibc build instead
# (same libc family, so no symbol mismatch). LWJGL 3 probes
# libopenal.so.1/libopenal.so, which must resolve under each of those names.
SYSTEM_OPENAL="${ROOTFS_DIR}/usr/lib/libopenal.so.1"
if [ ! -f "${SYSTEM_OPENAL}" ]; then
    echo "[!] Arch OpenAL library missing at /usr/lib/libopenal.so.1 despite ensure_pkg_mc openal" >&2
    exit 1
fi
for openal_name in libopenal.so libopenal.so.1 libopenal64.so; do
    cp -Lf "${SYSTEM_OPENAL}" "${MC_NATIVES_ROOTFS}/${openal_name}"
done
echo "[+] Replaced bundled OpenAL with Arch's glibc OpenAL Soft"

# Copy all library JARs into rootfs
cp -f "${LIBS_DIR}"/*.jar "${MC_LIBS_ROOTFS}/" 2>/dev/null || true

NATIVES_COUNT=$(find "${MC_NATIVES_ROOTFS}" -name '*.so' | wc -l)
LIBS_COUNT=$(find "${MC_LIBS_ROOTFS}" -name '*.jar' | wc -l)
echo "[+] Staged ${LIBS_COUNT} library JARs → /opt/minecraft/libs/"
echo "[+] Staged ${NATIVES_COUNT} native .so files → /opt/minecraft/natives/"

# Local assets use the official launcher layout: indexes/1.16.json and
# objects/<first-two-hash-chars>/<sha1>.
ASSETS_BUILD_DIR="${BUILD_DIR}/mc1165-assets"
MC_ASSETS_ROOTFS="${ROOTFS_DIR}/opt/minecraft/assets"
if [ ! -f "${MC_LOCAL_DIR}/assets/indexes/${MC_ASSET_INDEX}.json" ] || \
   [ ! -d "${MC_LOCAL_DIR}/assets/objects" ]; then
    echo "[!] Local asset index/objects missing. Expected ${MC_LOCAL_DIR}/assets/indexes/${MC_ASSET_INDEX}.json and objects/." >&2
    exit 1
fi
rm -rf "${ASSETS_BUILD_DIR}"
mkdir -p "${ASSETS_BUILD_DIR}"
cp -a "${MC_LOCAL_DIR}/assets/." "${ASSETS_BUILD_DIR}/"

rm -rf "${MC_ASSETS_ROOTFS}"
mkdir -p "${MC_ASSETS_ROOTFS}"
cp -a "${ASSETS_BUILD_DIR}/." "${MC_ASSETS_ROOTFS}/"
echo "[+] Staged Minecraft assets (all OGG sounds included) → /opt/minecraft/assets/"

# No native compatibility shim: the Arch rootfs is glibc, so Mojang's
# glibc-built LWJGL natives (including fortify __*_chk entry points and
# lazy PLT relocations) resolve against the rootfs libc directly.

# ─────────────────────────────────────────────────────────────────────────────
# 2. Stage minecraft.jar
# ─────────────────────────────────────────────────────────────────────────────
echo ""
echo "=== [2/4] Staging Minecraft ${MC_VERSION} client JAR ==="

jar_sha1() { sha1sum "$1" 2>/dev/null | awk '{print $1}'; }
jar_is_target() { [ "$(jar_sha1 "$1")" = "${MC_CLIENT_SHA1}" ]; }

MC_JAR_CACHE="${BUILD_DIR}/mc1165-client.jar"
MC_JAR_SRC=""
if [ -n "${MC_JAR_OVERRIDE}" ]; then
    [ -f "${MC_JAR_OVERRIDE}" ] || {
        echo "[!] --jar file not found: ${MC_JAR_OVERRIDE}" >&2
        exit 1
    }
    MC_JAR_SRC="${MC_JAR_OVERRIDE}"
    jar_is_target "${MC_JAR_SRC}" ||
        echo "[!] Warning: --jar SHA-1 differs from the official ${MC_VERSION} client JAR; continuing."
elif jar_is_target "${MC_JAR_CACHE}"; then
    MC_JAR_SRC="${MC_JAR_CACHE}"
elif jar_is_target "${MC_LOCAL_DIR}/client.jar"; then
    cp -f "${MC_LOCAL_DIR}/client.jar" "${MC_JAR_CACHE}"
    MC_JAR_SRC="${MC_JAR_CACHE}"
else
    echo "[!] Minecraft ${MC_VERSION} client JAR not found at ${MC_LOCAL_DIR}/client.jar. Supply it with --jar PATH." >&2
    exit 1
fi

mkdir -p "${ROOTFS_DIR}/opt/minecraft"
cp "${MC_JAR_SRC}" "${ROOTFS_DIR}/opt/minecraft/minecraft.jar"
echo "[+] Staged minecraft.jar (${MC_VERSION}) → /opt/minecraft/"

# ─────────────────────────────────────────────────────────────────────────────
# 3. Launcher script + desktop entries
# ─────────────────────────────────────────────────────────────────────────────
echo ""
echo "=== [3/4] Writing launcher + desktop entries ==="

mkdir -p "${ROOTFS_DIR}/usr/bin"
cat > "${ROOTFS_DIR}/usr/bin/minecraft" << LAUNCHER_EOF
#!/bin/sh
# Minecraft 1.16.5 launcher for AvoryOS

export DISPLAY="\${DISPLAY:-:0}"

# Hardware GL by default: on 'make run-amd-gpu' (VFIO AMD passthrough) Mesa
# loads radeonsi for the AMD render node and Xorg glamor is accelerated; on a
# plain 'make run' (virtio-vga only) Mesa falls back to llvmpipe automatically.
# The old launcher forced llvmpipe unconditionally, which is why F3 always
# showed "llvmpipe" even when radeonsi was available.
# Opt out: MC_SOFTWARE_GL=1 minecraft  (force llvmpipe)
# Optional overrides: MC_GALLIUM_DRIVER / MC_DRI_PRIME passthrough.
unset LIBGL_ALWAYS_SOFTWARE
unset GALLIUM_DRIVER
if [ "\${MC_SOFTWARE_GL:-0}" = "1" ]; then
    export LIBGL_ALWAYS_SOFTWARE=1
    export GALLIUM_DRIVER=llvmpipe
fi
if [ -n "\${MC_GALLIUM_DRIVER:-}" ]; then
    export GALLIUM_DRIVER="\${MC_GALLIUM_DRIVER}"
fi
if [ -n "\${MC_DRI_PRIME:-}" ]; then
    export DRI_PRIME="\${MC_DRI_PRIME}"
fi
# Keep the Mesa shader cache enabled for hardware drivers (the old launcher
# disabled it as llvmpipe tuning); only disable via MC_NO_SHADER_CACHE=1.
if [ "\${MC_NO_SHADER_CACHE:-0}" = "1" ]; then
    export MESA_SHADER_CACHE_DISABLE=true
    export MESA_GLSL_CACHE_DISABLE=true
fi
# Minecraft 1.16.5 renders via Blaze3D, which needs OpenGL 3.2 core / GLSL 150.
# Do not pin MESA_GL_VERSION_OVERRIDE or MESA_GLSL_VERSION_OVERRIDE here:
# llvmpipe already advertises OpenGL 4.5, and forcing 2.1/120 breaks shaders.
# AvoryOS exposes its HDA/AC97 audio through the OSS-compatible /dev/dsp.
# ALSOFT_CONF must be a config-file path, not inline configuration text.
# Restrict OpenAL Soft to OSS so it does not probe unsupported host backends.
export ALSOFT_DRIVERS="\${ALSOFT_DRIVERS:-oss}"

JAVA_HOME="${JAVA_HOME_GUEST}"
export PATH="\${JAVA_HOME}/bin:\${PATH}"
# The natives directory must be first: the loader resolves bare sonames such
# as libopenal.so through LD_LIBRARY_PATH when LWJGL/GLFW dlopen() them.
export LD_LIBRARY_PATH="/opt/minecraft/natives:\${JAVA_HOME}/lib:\${JAVA_HOME}/lib/server:/usr/lib:\${LD_LIBRARY_PATH:-}"

MC_USER="\${MC_USER:-${MC_USERNAME}}"
MC_RAM="\${MC_RAM:-1024m}"
MC_HOME="\${HOME}/.minecraft"
mkdir -p "\${MC_HOME}/saves" "\${MC_HOME}/resourcepacks"

# Build classpath: minecraft.jar + all libs
MC_CP="/opt/minecraft/minecraft.jar"
for jar in /opt/minecraft/libs/*.jar; do
    MC_CP="\${MC_CP}:\${jar}"
done

# Let Minecraft create options.txt with its own defaults and save changes
# made in the game. The launcher must not write presets or edit preferences.

echo "[minecraft] Starting Minecraft ${MC_VERSION} as '\${MC_USER}' (Heap: \${MC_RAM})..."
# glibc resolves Mojang's natives directly: no LD_PRELOAD shim, no
# LD_BIND_NOW override, no jemalloc MALLOC_CONF.
exec "\${JAVA_HOME}/bin/java" \
    -server \
    -Xms256m -Xmx"\${MC_RAM}" \
    -Xss1m \
    -XX:+UseParallelGC \
    -XX:ParallelGCThreads=2 \
    -XX:CICompilerCount=2 \
    -XX:+TieredCompilation \
    -XX:ReservedCodeCacheSize=48m \
    -XX:+DoEscapeAnalysis \
    -XX:+EliminateLocks \
    -XX:-UsePerfData \
    -XX:+UnlockDiagnosticVMOptions \
    -XX:-ImplicitNullChecks \
    --add-opens java.base/java.nio=ALL-UNNAMED \
    --add-opens java.base/java.lang=ALL-UNNAMED \
    --add-opens java.base/java.lang.reflect=ALL-UNNAMED \
    --add-opens java.base/java.util=ALL-UNNAMED \
    --add-opens java.base/sun.nio.ch=ALL-UNNAMED \
    --add-exports java.base/jdk.internal.misc=ALL-UNNAMED \
    --add-exports java.base/sun.security.action=ALL-UNNAMED \
    -Dio.netty.eventLoopThreads=2 \
    -Dhttp.keepAlive=false \
    -Dsun.net.client.defaultConnectTimeout=3000 \
    -Dsun.net.client.defaultReadTimeout=3000 \
    -Djdk.lang.Process.launchMechanism=posix_spawn \
    -Dminecraft.launcher.brand=AvoryOS \
    -Dminecraft.launcher.version=${MC_VERSION} \
    -Dorg.lwjgl.librarypath=/opt/minecraft/natives \
    -Dnet.java.games.input.librarypath=/opt/minecraft/natives \
    -Djava.library.path=/opt/minecraft/natives \
    -Dos.name=Linux \
    -cp "\${MC_CP}" \
    net.minecraft.client.main.Main \
    --username "\${MC_USER}" \
    --version "${MC_VERSION}" \
    --gameDir "\${MC_HOME}" \
    --assetsDir "/opt/minecraft/assets" \
    --assetIndex "${MC_ASSET_INDEX}" \
    --uuid "00000000-0000-0000-0000-000000000000" \
    --accessToken "0" \
    --userType "legacy" \
    --versionType "release"
LAUNCHER_EOF

chmod +x "${ROOTFS_DIR}/usr/bin/minecraft"
echo "[+] Wrote /usr/bin/minecraft"

# .desktop file
mkdir -p "${ROOTFS_DIR}/usr/share/applications"
# Drop the entry written by pre-1.16.5 setups.
rm -f "${ROOTFS_DIR}/usr/share/applications/minecraft-alpha.desktop"
cat > "${ROOTFS_DIR}/usr/share/applications/minecraft-${MC_VERSION}.desktop" << DESKTOP_EOF
[Desktop Entry]
Type=Application
Name=Minecraft ${MC_VERSION}
GenericName=Block Game
Comment=Minecraft ${MC_VERSION} Java Edition
Exec=minecraft
Icon=minecraft-alpha
Terminal=false
Categories=Game;
Keywords=minecraft;blocks;survival;
DESKTOP_EOF
echo "[+] Wrote minecraft-${MC_VERSION}.desktop"

# Openbox menu entry
OPENBOX_MENU="${ROOTFS_DIR}/etc/xdg/openbox/menu.xml"
if [ -f "${OPENBOX_MENU}" ]; then
    if grep -q 'st -e minecraft' "${OPENBOX_MENU}"; then
        sed -i "s|<item label=\"Minecraft [^\"]*\">|<item label=\"Minecraft ${MC_VERSION}\">|" \
            "${OPENBOX_MENU}"
        echo "[+] Updated Openbox menu.xml → Minecraft ${MC_VERSION}"
    elif ! grep -q 'minecraft' "${OPENBOX_MENU}"; then
        sed -i "s|<separator/>|<item label=\"Minecraft ${MC_VERSION}\">\n      <action name=\"Execute\"><execute>st -e minecraft</execute></action>\n    </item>\n    <separator/>|" \
            "${OPENBOX_MENU}"
        echo "[+] Patched Openbox menu.xml"
    fi
fi

# Java PATH profile
cat > "${ROOTFS_DIR}/etc/profile.d/java_avoryos.sh" << ENV_EOF
export JAVA_HOME="${JAVA_HOME_GUEST}"
export PATH="\${JAVA_HOME}/bin:\${PATH}"
ENV_EOF
chmod +x "${ROOTFS_DIR}/etc/profile.d/java_avoryos.sh"
echo "[+] Wrote /etc/profile.d/java_avoryos.sh"

# ─────────────────────────────────────────────────────────────────────────────
# Optional disk expansion
# ─────────────────────────────────────────────────────────────────────────────
if [ "${EXPAND_DISK}" = "1" ]; then
    CURRENT_SIZE=$(stat -c%s "${DISK_IMG}")
    TARGET_SIZE=$((8 * 1024 * 1024 * 1024))
    if [ "${CURRENT_SIZE}" -ge "${TARGET_SIZE}" ]; then
        echo "[*] disk.img already >= 8 GiB. Skipping."
    else
        echo "[*] Expanding disk.img to 8 GiB..."
        truncate -s 8G "${DISK_IMG}"
        PART_IMG="${BUILD_DIR}/part_expand.img"
        dd if="${DISK_IMG}" of="${PART_IMG}" bs=1M skip=1 status=none
        e2fsck -f -y "${PART_IMG}" || true
        resize2fs "${PART_IMG}"
        dd if="${PART_IMG}" of="${DISK_IMG}" bs=1M seek=1 conv=notrunc status=none
        rm -f "${PART_IMG}"
        echo "[+] disk.img expanded."
    fi
fi

# ─────────────────────────────────────────────────────────────────────────────
# 4. Inject into disk.img
# ─────────────────────────────────────────────────────────────────────────────
echo ""
echo "=== [4/4] Injecting into disk.img ==="

PART_IMG="${BUILD_DIR}/part_mc.img"
dd if="${DISK_IMG}" of="${PART_IMG}" bs=1M skip=1 status=none
"${POPULATE_SCRIPT}" "${PART_IMG}" "${ROOTFS_DIR}" "/"
dd if="${PART_IMG}" of="${DISK_IMG}" bs=1M seek=1 conv=notrunc status=none
rm -f "${PART_IMG}"

echo ""
echo "╔══════════════════════════════════════════════════════════════════╗"
echo "║  [SUCCESS] Minecraft 1.16.5 is ready in AvoryOS!                 ║"
echo "╠══════════════════════════════════════════════════════════════════╣"
echo "║  Boot AvoryOS → open a terminal → type:  minecraft               ║"
echo "║  radeonsi needs the VFIO GPU boot:  make run-amd-gpu             ║"
echo "║  (plain 'make run' has no GPU, so Mesa falls back to llvmpipe)    ║"
echo "║  Force software GL:  MC_SOFTWARE_GL=1 minecraft                   ║"
echo "║  Custom username:  MC_USER=YourName minecraft                    ║"
echo "║  Custom RAM:       MC_RAM=1536m minecraft                        ║"
echo "║  Assets incl. 2,336 OGG sounds staged in /opt/minecraft/assets   ║"
echo "╚══════════════════════════════════════════════════════════════════╝"
