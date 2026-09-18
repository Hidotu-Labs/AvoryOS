#!/usr/bin/env bash
# scripts/setup-mocktail.sh - Downloads and installs Mocktail AppImage into AvoryOS disk image
#
# Mocktail is an experimental Roblox compatibility runtime that runs the Android
# x86_64 Roblox client on Linux via SDL3 + OpenGL/EGL.
#
# Usage:
#   ./scripts/setup-mocktail.sh
#
# Prerequisites:
#   1. Run ./scripts/setup-alpine.sh first (installs runtime deps in rootfs)
#   2. Host system needs: wget or curl

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DISK_IMG="${ROOT_DIR}/disk.img"
BUILD_DIR="${ROOT_DIR}/build/alpine"
ROOTFS_DIR="${BUILD_DIR}/rootfs"
POPULATE_SCRIPT="${ROOT_DIR}/scripts/populate-ext2-dir.sh"
MOCKTAIL_BUILD_DIR="${BUILD_DIR}/mocktail-build"
HOST_DATA_DIR="${ROOT_DIR}/build/mocktail-host-data"

SUDO=""
if [ "$(id -u)" -ne 0 ] && ! [ -w "${ROOTFS_DIR}" ]; then
    SUDO="sudo"
fi

# ─────────────────────────────────────────────────────────────────────────────
# Sanity checks
# ─────────────────────────────────────────────────────────────────────────────
[ ! -d "${ROOTFS_DIR}/etc" ] && {
    echo "[!] Alpine rootfs not found. Run ./scripts/setup-alpine.sh first."
    exit 1
}
[ ! -f "${DISK_IMG}" ] && {
    echo "[!] disk.img not found. Run 'make disk.img' first."
    exit 1
}

# Check for download tool
DOWNLOAD_CMD=""
if command -v wget >/dev/null 2>&1; then
    DOWNLOAD_CMD="wget -O"
elif command -v curl >/dev/null 2>&1; then
    DOWNLOAD_CMD="curl -L -o"
else
    echo "[!] Neither wget nor curl found. Install one of them."
    exit 1
fi

mkdir -p "${MOCKTAIL_BUILD_DIR}"

# ─────────────────────────────────────────────────────────────────────────────
# 1. Download Mocktail AppImage
# ─────────────────────────────────────────────────────────────────────────────
echo ""
echo "=== [1/4] Downloading Mocktail AppImage ==="

APPIMAGE_URL="https://github.com/komaruworld/mocktail/releases/latest/download/mocktail-x86_64.AppImage"
APPIMAGE_FILE="${MOCKTAIL_BUILD_DIR}/mocktail.AppImage"

if [ ! -f "${APPIMAGE_FILE}" ]; then
    echo "[*] Downloading Mocktail AppImage from GitHub releases..."
    ${DOWNLOAD_CMD} "${APPIMAGE_FILE}" "${APPIMAGE_URL}"
    chmod +x "${APPIMAGE_FILE}"
    echo "[+] AppImage downloaded"
else
    echo "[*] AppImage already exists, skipping download"
fi

# ─────────────────────────────────────────────────────────────────────────────
# 2. Extract AppImage contents
# ─────────────────────────────────────────────────────────────────────────────
echo ""
echo "=== [2/4] Extracting AppImage ==="

EXTRACT_DIR="${MOCKTAIL_BUILD_DIR}/mocktail-extracted"
rm -rf "${EXTRACT_DIR}"
mkdir -p "${EXTRACT_DIR}"

echo "[*] Extracting AppImage contents..."
cd "${EXTRACT_DIR}"
"${APPIMAGE_FILE}" --appimage-extract >/dev/null 2>&1 || {
    echo "[!] Failed to extract AppImage. Trying alternative method..."
    # Alternative: manually extract (AppImages are ISO9660 + offset)
    OFFSET=$(LC_ALL=C grep -aobm1 "ELF" "${APPIMAGE_FILE}" | cut -d: -f1)
    if [ -n "${OFFSET}" ]; then
        dd if="${APPIMAGE_FILE}" bs=1 skip="${OFFSET}" of="${EXTRACT_DIR}/mocktail.elf" 2>/dev/null
        chmod +x "${EXTRACT_DIR}/mocktail.elf"
        echo "[+] Extracted ELF binary directly"
    else
        echo "[!] Could not extract AppImage. Manual installation required."
        exit 1
    fi
}

# AppImage extracts to squashfs-root/
if [ -d "squashfs-root" ]; then
    echo "[*] Copying AppImage contents to rootfs..."
    
    # Install the main binary
    ${SUDO} mkdir -p "${ROOTFS_DIR}/usr/bin"
    ${SUDO} mkdir -p "${ROOTFS_DIR}/opt/mocktail"
    
    # Ensure write permissions on existing files so read-only assets don't fail overwrite
    [ -d "${ROOTFS_DIR}/opt/mocktail" ] && chmod -R u+w "${ROOTFS_DIR}/opt/mocktail" 2>/dev/null || true

    # Copy all AppImage contents to /opt/mocktail
    ${SUDO} cp -rf --remove-destination squashfs-root/* "${ROOTFS_DIR}/opt/mocktail/"
    
    # Fix permissions for all executables and scripts
    ${SUDO} find "${ROOTFS_DIR}/opt/mocktail" -type f -name "*.sh" -exec chmod +x {} \;
    ${SUDO} find "${ROOTFS_DIR}/opt/mocktail" -type f -name "AppRun" -exec chmod +x {} \;
    ${SUDO} find "${ROOTFS_DIR}/opt/mocktail/usr/bin" -type f -exec chmod +x {} \; 2>/dev/null || true
    ${SUDO} find "${ROOTFS_DIR}/opt/mocktail/usr/share/mocktail-bundle" -type f -name "*.sh" -exec chmod +x {} \; 2>/dev/null || true
    
    # Replace mocktail_failure_dialog with a lightweight stub so it won't crash,
    # require heavy GTK4/Libadwaita dependencies, or trigger IFUNC relink errors
    DIALOG_BIN="${ROOTFS_DIR}/opt/mocktail/usr/share/mocktail-bundle/mocktail/bin/mocktail_failure_dialog"
    if [ -f "${DIALOG_BIN}" ] || [ -f "${DIALOG_BIN}.real" ]; then
        ${SUDO} rm -f "${DIALOG_BIN}" "${DIALOG_BIN}.real"
        ${SUDO} tee "${DIALOG_BIN}" > /dev/null << 'WRAPPER_EOF'
#!/bin/sh
# Mocktail failure and progress dialog stub for AvoryOS
case "$1" in
    --message|--warning)
        echo "[mocktail] $1: $2" >&2
        ;;
    --monitor|--progress-monitor)
        cat >/dev/null 2>&1
        ;;
    *)
        [ -n "$*" ] && echo "[mocktail] Dialog: $*" >&2
        ;;
esac
exit 0
WRAPPER_EOF
        ${SUDO} chmod +x "${DIALOG_BIN}"
    fi

    # Stub mocktail_updater to prevent hanging on online update preflight
    IN_GUEST_UPDATER="${ROOTFS_DIR}/opt/mocktail/usr/share/mocktail-bundle/mocktail/bin/mocktail_updater"
    if [ -f "${IN_GUEST_UPDATER}" ] && [ ! -f "${IN_GUEST_UPDATER}.real" ]; then
        ${SUDO} cp -f "${IN_GUEST_UPDATER}" "${IN_GUEST_UPDATER}.real"
        ${SUDO} tee "${IN_GUEST_UPDATER}" > /dev/null << 'UPDATER_STUB_EOF'
#!/bin/sh
# Mocktail updater stub for AvoryOS - prevents hanging on online update checks
case "$*" in
    *verify-current*)
        exec "${0}.real" "$@"
        ;;
    *)
        exit 0
        ;;
esac
UPDATER_STUB_EOF
        ${SUDO} chmod +x "${IN_GUEST_UPDATER}"
    fi

    # Patch update_roblox_payload.sh to bypass APK signature verification
    UPDATE_SCRIPT="${ROOTFS_DIR}/opt/mocktail/usr/share/mocktail-bundle/mocktail/scripts/update_roblox_payload.sh"
    if [ -f "${UPDATE_SCRIPT}" ]; then
        ${SUDO} sed -i 's|apksigner verify.*||g' "${UPDATE_SCRIPT}"
        ${SUDO} sed -i 's|die "APK signing certificate is not trusted for com.roblox.client"|true|g' "${UPDATE_SCRIPT}"
        ${SUDO} sed -i 's|die "base and split APKs do not share a signing certificate"|true|g' "${UPDATE_SCRIPT}"
        ${SUDO} sed -i 's|die "base APK signature verification failed"|true|g' "${UPDATE_SCRIPT}"
        ${SUDO} sed -i 's|die "split APK signature verification failed"|true|g' "${UPDATE_SCRIPT}"
    fi

    # Ensure glibc DNS resolver libraries and resolv.conf exist in rootfs
    ${SUDO} cp -f "${ROOT_DIR}/toolchain/glibc-sysroot/lib/libnss_dns.so.2" "${ROOTFS_DIR}/lib64/" 2>/dev/null || true
    ${SUDO} cp -f "${ROOT_DIR}/toolchain/glibc-sysroot/lib/libnss_files.so.2" "${ROOTFS_DIR}/lib64/" 2>/dev/null || true
    ${SUDO} cp -f "${ROOT_DIR}/toolchain/glibc-sysroot/lib/libnss_dns.so.2" "${ROOTFS_DIR}/lib/" 2>/dev/null || true
    ${SUDO} cp -f "${ROOT_DIR}/toolchain/glibc-sysroot/lib/libnss_files.so.2" "${ROOTFS_DIR}/lib/" 2>/dev/null || true
    ${SUDO} tee "${ROOTFS_DIR}/etc/resolv.conf" > /dev/null << 'RESOLV_EOF'
nameserver 10.0.2.3
nameserver 1.1.1.1
RESOLV_EOF

    echo "[+] AppImage contents installed to /opt/mocktail (signature checks bypassed)"
else
    # Fallback: AppImage didn't extract properly
    echo "[!] No squashfs-root found after extraction"
    exit 1
fi

cd "${ROOT_DIR}"

# ─────────────────────────────────────────────────────────────────────────────
# 3. Download & extract Roblox payload on host
# ─────────────────────────────────────────────────────────────────────────────
echo ""
echo "=== [3/4] Pre-installing Roblox Payload (Host-Side) ==="

BUNDLE_DIR="${ROOTFS_DIR}/opt/mocktail/usr/share/mocktail-bundle/mocktail"
UPDATER_BIN="${BUNDLE_DIR}/bin/mocktail_updater"

NEED_DOWNLOAD=1
if [ -f "${HOST_DATA_DIR}/mocktail/current.json" ]; then
    CURRENT_PAYLOAD_PATH=$(grep -o '"payload_path": "[^"]*"' "${HOST_DATA_DIR}/mocktail/current.json" 2>/dev/null | cut -d'"' -f4 || true)
    if [ -n "${CURRENT_PAYLOAD_PATH}" ] && [ -f "${HOST_DATA_DIR}/mocktail/${CURRENT_PAYLOAD_PATH}/libroblox.so" ]; then
        echo "[*] Found existing valid Roblox payload in cache: ${CURRENT_PAYLOAD_PATH}"
        NEED_DOWNLOAD=0
    fi
fi

if [ "${NEED_DOWNLOAD}" -eq 1 ]; then
    echo "[*] Running mocktail_updater on host to download supported Roblox payload..."
    if [ ! -x "${UPDATER_BIN}" ]; then
        echo "[!] mocktail_updater binary not found at ${UPDATER_BIN}"
        exit 1
    fi

    LD_LIBRARY_PATH="${BUNDLE_DIR}/lib:${BUNDLE_DIR}/bin:/lib64:/usr/lib64:${LD_LIBRARY_PATH:-}" \
    MOCKTAIL_PROJECT_ROOT="${BUNDLE_DIR}" \
    MOCKTAIL_COMPATIBILITY_MANIFEST="${BUNDLE_DIR}/metadata/roblox_compatibility.json" \
    MOCKTAIL_UPDATE_COMPATIBILITY_PATH="${BUNDLE_DIR}/metadata/roblox_compatibility.json" \
    MOCKTAIL_UPDATE_SIGNING_TRUST_PATH="${BUNDLE_DIR}/metadata/roblox_signing_certificates.json" \
    MOCKTAIL_UPDATE_HOST_ABI_REFERENCE="${BUNDLE_DIR}/metadata/roblox_host_abi_reference.json" \
    MOCKTAIL_BOOTSTRAP_SOURCES_PATH="${BUNDLE_DIR}/metadata/roblox_bootstrap_sources.json" \
    XDG_DATA_HOME="${HOST_DATA_DIR}" \
    "${UPDATER_BIN}" update --skip-canary || {
        echo "[!] mocktail_updater failed during host-side download"
        exit 1
    }
    echo "[+] Download and extraction complete on host"
fi

# Clean up failed / inactive candidate payloads to conserve disk space
if [ -d "${HOST_DATA_DIR}/mocktail/payloads" ] && [ -f "${HOST_DATA_DIR}/mocktail/current.json" ]; then
    ACTIVE_PAYLOAD=$(grep -o '"payload_id": "[^"]*"' "${HOST_DATA_DIR}/mocktail/current.json" 2>/dev/null | cut -d'"' -f4 || true)
    for pdir in "${HOST_DATA_DIR}/mocktail/payloads"/*; do
        if [ -d "${pdir}" ]; then
            pname=$(basename "${pdir}")
            if [ -n "${ACTIVE_PAYLOAD}" ] && [ "${pname}" != "${ACTIVE_PAYLOAD}" ]; then
                echo "[*] Cleaning up inactive payload candidate: ${pname}"
                rm -rf "${pdir}"
            fi
        fi
    done
fi

# Copy payload to /opt/mocktail/data (shared system location, no duplication)
echo "[*] Installing Roblox payload into /opt/mocktail/data..."
${SUDO} mkdir -p "${ROOTFS_DIR}/opt/mocktail"
${SUDO} rm -rf "${ROOTFS_DIR}/opt/mocktail/data"
${SUDO} cp -rf "${HOST_DATA_DIR}/mocktail" "${ROOTFS_DIR}/opt/mocktail/data"
${SUDO} chmod -R a+rX "${ROOTFS_DIR}/opt/mocktail/data"

# Pre-cache NotoSansCJK fallback font so Roblox never downloads 16MB CJK font over network
for pdir in "${ROOTFS_DIR}/opt/mocktail/data/payloads"/*; do
    if [ -d "${pdir}/assets" ]; then
        FONT_DST="${pdir}/assets/fonts/NotoSansCJK-Regular.otf"
        JSON_DST="${pdir}/assets/content/fonts/families/NotoSansCJKFallback.json"
        if [ ! -f "${FONT_DST}" ]; then
            echo "[*] Downloading NotoSansCJK font (asset 123924484074402) on host..."
            LOC=$(curl -s "https://assetdelivery.roblox.com/v2/asset?id=123924484074402" | grep -o '"location":"[^"]*"' | cut -d'"' -f4 || true)
            if [ -n "${LOC}" ]; then
                curl -sL "${LOC}" | gzip -dc > "${FONT_DST}" 2>/dev/null || true
            fi
        fi
        if [ -f "${FONT_DST}" ] && [ -f "${JSON_DST}" ]; then
            sed -i 's|"rbxassetid://123924484074402"|"rbxasset://fonts/NotoSansCJK-Regular.otf"|g' "${JSON_DST}"
            echo "[+] Pre-cached NotoSansCJK font into payload: ${FONT_DST}"
        fi
    fi
done

# Link active payload to /opt/mocktail/roblox for instant startup without update checks
ACTIVE_PAYLOAD_PATH=$(grep -o '"payload_path": "[^"]*"' "${ROOTFS_DIR}/opt/mocktail/data/current.json" 2>/dev/null | cut -d'"' -f4 || true)
if [ -n "${ACTIVE_PAYLOAD_PATH}" ]; then
    ${SUDO} ln -sfn "data/${ACTIVE_PAYLOAD_PATH}" "${ROOTFS_DIR}/opt/mocktail/roblox"
    echo "[+] Linked active Roblox payload to /opt/mocktail/roblox"
fi

# Create default configuration in /opt/mocktail/config
${SUDO} mkdir -p "${ROOTFS_DIR}/opt/mocktail/config"
${SUDO} tee "${ROOTFS_DIR}/opt/mocktail/config/config.yaml" > /dev/null << 'CONFIG_EOF'
version: 1
device: pc-windows-11
runtime:
  headless: false
graphics:
  backend: opengl
  vsync: off
performance:
  multithreaded_rendering: true
  physics_worker_mode: throughput
  memory_limit_mb: 0
  gamemode: off
audio:
  output_device: default
window:
  width: 1280
  height: 720
  title: Roblox
CONFIG_EOF
${SUDO} chmod 644 "${ROOTFS_DIR}/opt/mocktail/config/config.yaml"

${SUDO} tee "${ROOTFS_DIR}/opt/mocktail/config/fflags.json" > /dev/null << 'FFLAGS_EOF'
{
  "FFlagDebugGraphicsPreferVulkan": "False",
  "FFlagDebugGraphicsPreferOpenGL": "True",
  "DFIntTaskSchedulerTargetFps": "60",
  "FFlagDebugDisableAudio": "True",
  "FFlagDisablePostFx": "True",
  "FIntRenderShadowIntensity": "0",
  "FFlagGlobalWindRendering": "False",
  "FFlagPreloadAllFonts": "False",
  "DFFlagTextureQualityOverrideEnabled": "True",
  "DFIntTextureQualityOverride": "1",
  "FFlagDataModelPatchEnableOTAPolling": "False",
  "DFFlagDataModelPatchEnableOTAPolling": "False",
  "DFIntDataModelPatchEnableOTAPollingPercentage": "0",
  "FFlagDownloadNewPatch": "False",
  "DFFlagDownloadNewPatch": "False",
  "FFlagDownloadNewPatchAssets": "False",
  "DFFlagDownloadNewPatchAssets": "False",
  "FFlagUpdatePatch": "False",
  "DFFlagUpdatePatch": "False",
  "FFlagRobloxLuaOTA": "False",
  "DFFlagRobloxLuaOTA": "False",
  "FFlagForceOTAUpdate": "False",
  "DFFlagForceOTAUpdate": "False",
  "FFlagEnableSeamlessForceOTAUpdate2": "False",
  "DFFlagEnableSeamlessForceOTAUpdate2": "False",
  "FFlagDataModelPatcherForceLocal": "True",
  "DFFlagDataModelPatcherForceLocal": "True",
  "DFIntDataModelPatcherForceLocalPercentage": "100",
  "FFlagDebugForceLocalPatchConfigVersion": "True",
  "DFFlagDebugForceLocalPatchConfigVersion": "True"
}
FFLAGS_EOF
${SUDO} chmod 644 "${ROOTFS_DIR}/opt/mocktail/config/fflags.json"

# Create symlinks and default configs for root and user avory
${SUDO} mkdir -p "${ROOTFS_DIR}/root/.local/share"
${SUDO} rm -rf "${ROOTFS_DIR}/root/.local/share/mocktail"
${SUDO} ln -sfn /opt/mocktail/data "${ROOTFS_DIR}/root/.local/share/mocktail"

${SUDO} mkdir -p "${ROOTFS_DIR}/root/.config/mocktail"
${SUDO} cp -f "${ROOTFS_DIR}/opt/mocktail/config/config.yaml" "${ROOTFS_DIR}/root/.config/mocktail/config.yaml"
${SUDO} cp -f "${ROOTFS_DIR}/opt/mocktail/config/fflags.json" "${ROOTFS_DIR}/root/.config/mocktail/fflags.json"

${SUDO} mkdir -p "${ROOTFS_DIR}/home/avory/.local/share"
${SUDO} rm -rf "${ROOTFS_DIR}/home/avory/.local/share/mocktail"
${SUDO} ln -sfn /opt/mocktail/data "${ROOTFS_DIR}/home/avory/.local/share/mocktail"

${SUDO} mkdir -p "${ROOTFS_DIR}/home/avory/.config/mocktail"
${SUDO} cp -f "${ROOTFS_DIR}/opt/mocktail/config/config.yaml" "${ROOTFS_DIR}/home/avory/.config/mocktail/config.yaml"
${SUDO} cp -f "${ROOTFS_DIR}/opt/mocktail/config/fflags.json" "${ROOTFS_DIR}/home/avory/.config/mocktail/config.json" 2>/dev/null || true
${SUDO} cp -f "${ROOTFS_DIR}/opt/mocktail/config/fflags.json" "${ROOTFS_DIR}/home/avory/.config/mocktail/fflags.json"
${SUDO} chown -R 1000:1000 "${ROOTFS_DIR}/home/avory/.local" "${ROOTFS_DIR}/home/avory/.config" 2>/dev/null || true

${SUDO} mkdir -p "${ROOTFS_DIR}/.local/share"
${SUDO} rm -rf "${ROOTFS_DIR}/.local/share/mocktail"
${SUDO} ln -sfn /opt/mocktail/data "${ROOTFS_DIR}/.local/share/mocktail"

${SUDO} mkdir -p "${ROOTFS_DIR}/.config/mocktail"
${SUDO} cp -f "${ROOTFS_DIR}/opt/mocktail/config/config.yaml" "${ROOTFS_DIR}/.config/mocktail/config.yaml"
${SUDO} cp -f "${ROOTFS_DIR}/opt/mocktail/config/fflags.json" "${ROOTFS_DIR}/.config/mocktail/fflags.json"
echo "[+] Roblox payload installed in /opt/mocktail/data"

# ─────────────────────────────────────────────────────────────────────────────
# 4. Create launcher script, desktop entry, and inject into disk.img
# ─────────────────────────────────────────────────────────────────────────────
echo ""
echo "=== [4/4] Creating launcher and injecting into disk.img ==="

# Create wrapper script
${SUDO} tee "${ROOTFS_DIR}/usr/bin/mocktail" > /dev/null << 'LAUNCHER_EOF'
#!/bin/sh
# Mocktail fast direct launcher for AvoryOS

export DISPLAY="${DISPLAY:-:0}"
export APPDIR="/opt/mocktail"

BUNDLE_ROOT="/opt/mocktail/usr/share/mocktail-bundle"
RUNTIME_ROOT="${BUNDLE_ROOT}/mocktail"
SUPPORT_ROOT="${RUNTIME_ROOT}/runtime"

export PATH="${SUPPORT_ROOT}/bin:${RUNTIME_ROOT}/runtime/android-tools/bin:${PATH}"
export LD_LIBRARY_PATH="${RUNTIME_ROOT}/lib:${RUNTIME_ROOT}/bin:/lib64:/usr/lib64:/lib:/usr/lib:${LD_LIBRARY_PATH}"

# Mocktail configuration paths
export MOCKTAIL_FREEBSD_SOCKET_HELPER="${RUNTIME_ROOT}/bin/mocktail_freebsd_socket_helper"
export MOCKTAIL_PROJECT_ROOT="${RUNTIME_ROOT}"
export MOCKTAIL_COMPATIBILITY_MANIFEST="${RUNTIME_ROOT}/metadata/roblox_compatibility.json"
export MOCKTAIL_UPDATE_COMPATIBILITY_PATH="${RUNTIME_ROOT}/metadata/roblox_compatibility.json"
export MOCKTAIL_UPDATE_SIGNING_TRUST_PATH="${RUNTIME_ROOT}/metadata/roblox_signing_certificates.json"
export MOCKTAIL_UPDATE_HOST_ABI_REFERENCE="${RUNTIME_ROOT}/metadata/roblox_host_abi_reference.json"
export MOCKTAIL_BOOTSTRAP_SOURCES_PATH="${RUNTIME_ROOT}/metadata/roblox_bootstrap_sources.json"
export MOCKTAIL_UPDATE_HELPER="${RUNTIME_ROOT}/bin/mocktail_updater"
export MOCKTAIL_UPDATE_CANARY_BIN="${RUNTIME_ROOT}/bin/mocktail"
export MOCKTAIL_UPDATE_SMOKE_SCRIPT="${RUNTIME_ROOT}/scripts/real_bringup_smoke.sh"
export MOCKTAIL_BIN="${RUNTIME_ROOT}/bin/mocktail"
export MOCKTAIL_PORTABLE_MODE="standalone"

# Python & Java runtimes
export PYTHONHOME="${SUPPORT_ROOT}/python"
unset PYTHONPATH PYTHONUSERBASE
export PYTHONNOUSERSITE=1 PYTHONDONTWRITEBYTECODE=1
export JAVA_HOME="${SUPPORT_ROOT}/jre"
export SSL_CERT_FILE="${SUPPORT_ROOT}/share/ca-certificates/ca-bundle.crt"
export REQUESTS_CA_BUNDLE="${SSL_CERT_FILE}"

# WebKit & GStreamer configuration
export WEBKIT_EXEC_PATH="${RUNTIME_ROOT}/libexec/webkitgtk-6.0"
export WEBKIT_INJECTED_BUNDLE_PATH="${RUNTIME_ROOT}/lib/webkitgtk-6.0/injected-bundle"
export GST_PLUGIN_PATH_1_0="${RUNTIME_ROOT}/lib/plugins/gstreamer-1.0"
export GST_PLUGIN_SYSTEM_PATH_1_0=""
export GST_PLUGIN_SCANNER="${RUNTIME_ROOT}/libexec/gstreamer-1.0/gst-plugin-scanner"
export GIO_EXTRA_MODULES="${RUNTIME_ROOT}/lib/plugins/gio/modules"
export GIO_USE_TLS="gnutls"
export GDK_PIXBUF_MODULE_FILE="${RUNTIME_ROOT}/lib/plugins/gdk-pixbuf-2.0/2.10.0/loaders.cache"
export GLYCIN_DATA_DIR="${RUNTIME_ROOT}/share/glycin-loaders/2+"
export GSETTINGS_SCHEMA_DIR="${RUNTIME_ROOT}/share/glib-2.0/schemas"
export XDG_DATA_DIRS="${RUNTIME_ROOT}/share${XDG_DATA_DIRS:+:${XDG_DATA_DIRS}}"

# Disable sandboxes that require unshare / user namespaces
export WEBKIT_DISABLE_SANDBOX_THIS_IS_DANGEROUS=1
export WEBKIT_FORCE_SANDBOX=0
export MOCKTAIL_SKIP_NAMESPACE_CHECK=1
export MOCKTAIL_STANDALONE_NAMESPACE=1
export MOCKTAIL_SKIP_HOST_CHECK=1
export MOCKTAIL_SKIP_UPDATE_CHECK=1
export MOCKTAIL_DATA_ROOT="/opt/mocktail/data"

# OpenGL/EGL configuration (software rendering via llvmpipe)
export LIBGL_ALWAYS_SOFTWARE=1
export GALLIUM_DRIVER=llvmpipe
export MESA_LOADER_DRIVER_OVERRIDE=llvmpipe
export LIBGL_DRIVERS_PATH=/usr/lib/xorg/modules/dri
export LP_NUM_THREADS=$(nproc 2>/dev/null || echo 4)
export MESA_NO_ERROR=1
export MESA_GL_VERSION_OVERRIDE=3.3
export MESA_GLSL_VERSION_OVERRIDE=330

# Disable broken GTK4 failure dialog
export MOCKTAIL_DISABLE_FAILURE_DIALOG=1

# SDL / X11 Windowing
export SDL_VIDEO_DRIVER=x11
export MOCKTAIL_FORCE_X11=1
export MOCKTAIL_DEBUG_SHOW_WINDOW_BEFORE_FRAME=1
export MOCKTAIL_HIDE_WINDOW_UNTIL_FIRST_SWAP=0

# Disable audio for now (SDL dummy driver)
export SDL_AUDIODRIVER=dummy

# XDG directories
export XDG_CONFIG_HOME="${XDG_CONFIG_HOME:-${HOME:-/root}/.config}"
export XDG_DATA_HOME="${XDG_DATA_HOME:-${HOME:-/root}/.local/share}"
export XDG_CACHE_HOME="${XDG_CACHE_HOME:-${HOME:-/root}/.cache}"
mkdir -p "${XDG_CONFIG_HOME}/mocktail" "${XDG_DATA_HOME}" "${XDG_CACHE_HOME}"

# If no user payload, link to shared system payload
MOCKTAIL_SYSTEM_DATA="/opt/mocktail/data"
MOCKTAIL_USER_DATA="${XDG_DATA_HOME}/mocktail"
if [ ! -f "${MOCKTAIL_USER_DATA}/current.json" ] && [ -f "${MOCKTAIL_SYSTEM_DATA}/current.json" ]; then
    mkdir -p "${XDG_DATA_HOME}"
    rm -rf "${MOCKTAIL_USER_DATA}"
    ln -sfn "${MOCKTAIL_SYSTEM_DATA}" "${MOCKTAIL_USER_DATA}"
fi

# Ensure user config.yaml is cleanly installed without directory collisions
rm -rf "${XDG_CONFIG_HOME}/mocktail/config.yaml"
if [ -f "/opt/mocktail/config/config.yaml" ]; then
    cp -f /opt/mocktail/config/config.yaml "${XDG_CONFIG_HOME}/mocktail/config.yaml"
fi
if [ -f "${XDG_CONFIG_HOME}/mocktail/config.yaml" ]; then
    sed -i 's/backend:[[:space:]]*direct-vulkan/backend: opengl/g' "${XDG_CONFIG_HOME}/mocktail/config.yaml" 2>/dev/null || true
    sed -i '/roblox_library:/d' "${XDG_CONFIG_HOME}/mocktail/config.yaml" 2>/dev/null || true
fi

# Sync default fflags to user config so patch-disabling flags and optimizations are always applied
rm -rf "${XDG_CONFIG_HOME}/mocktail/fflags.json"
if [ -f "/opt/mocktail/config/fflags.json" ]; then
    mkdir -p "${XDG_CONFIG_HOME}/mocktail"
    cp -f /opt/mocktail/config/fflags.json "${XDG_CONFIG_HOME}/mocktail/fflags.json"
fi

# Ensure Roblox cache directory exists
mkdir -p "${XDG_DATA_HOME}/mocktail/android/data/user/0/com.roblox.client/cache" 2>/dev/null || true

# Pass --graphics opengl if not explicitly specified
has_graphics=0
for arg in "$@"; do
    case "$arg" in
        --graphics*) has_graphics=1 ;;
    esac
done
if [ "$has_graphics" -eq 0 ]; then
    set -- --graphics opengl "$@"
fi

# AvoryOS: a killed Mocktail run can leave its external-launch socket behind.
# The broker only unlinks sockets it considers stale, and a bind() over the
# leftover node has failed with EINVAL here ("cannot activate external-launch
# socket: Invalid argument"), aborting startup before any window exists.
# Clear the per-user endpoint directory when no other instance is alive.
ENDPOINT_DIR="/tmp/mocktail-$(id -u 2>/dev/null || echo 0)"
if ! pgrep -f "mocktail-bundle/mocktail/bin/mocktail" >/dev/null 2>&1; then
    rm -rf "${ENDPOINT_DIR}" 2>/dev/null || true
fi

echo "[mocktail] Starting Mocktail Roblox runtime..."

if [ -x "${RUNTIME_ROOT}/bin/mocktail" ]; then
    cd "${RUNTIME_ROOT}"
    exec "${RUNTIME_ROOT}/bin/mocktail" "$@"
elif [ -x "$APPDIR/AppRun" ]; then
    cd "$APPDIR"
    exec "$APPDIR/AppRun" "$@"
else
    echo "[mocktail] ERROR: Could not find mocktail executable"
    exit 1
fi
LAUNCHER_EOF

${SUDO} chmod +x "${ROOTFS_DIR}/usr/bin/mocktail"
echo "[+] Created /usr/bin/mocktail launcher"

# Desktop entry
${SUDO} mkdir -p "${ROOTFS_DIR}/usr/share/applications"
${SUDO} tee "${ROOTFS_DIR}/usr/share/applications/mocktail.desktop" > /dev/null << 'DESKTOP_EOF'
[Desktop Entry]
Type=Application
Name=Mocktail
GenericName=Roblox Client
Comment=Android Roblox client runtime for Linux
Exec=mocktail
Icon=mocktail
Terminal=false
Categories=Game;
Keywords=roblox;mocktail;android;
DESKTOP_EOF
echo "[+] Created mocktail.desktop"

# Add to Openbox menu if it exists
OPENBOX_MENU="${ROOTFS_DIR}/etc/xdg/openbox/menu.xml"
if [ -f "${OPENBOX_MENU}" ] && ! ${SUDO} grep -q 'mocktail' "${OPENBOX_MENU}"; then
    ${SUDO} sed -i 's|<separator/>|<item label="Mocktail (Roblox)">\n      <action name="Execute"><execute>mocktail</execute></action>\n    </item>\n    <separator/>|' \
        "${OPENBOX_MENU}"
    echo "[+] Added Mocktail to Openbox menu"
fi

# Inject into disk.img
echo "[*] Injecting into disk.img..."
PART_IMG="${BUILD_DIR}/part_mocktail.img"
dd if="${DISK_IMG}" of="${PART_IMG}" bs=1M skip=1 status=none

echo "[*] Populating partition with Mocktail installation..."
"${POPULATE_SCRIPT}" "${PART_IMG}" "${ROOTFS_DIR}/opt/mocktail" "/opt/mocktail"

# Inject symlinks and single files directly via debugfs
DEBUGFS_BIN="debugfs"
[ -x /run/host/usr/bin/debugfs ] && DEBUGFS_BIN=/run/host/usr/bin/debugfs

"${DEBUGFS_BIN}" -w "${PART_IMG}" << EOF >/dev/null 2>&1
cd /
rm /.local/share/mocktail
mkdir /.local
mkdir /.local/share
symlink /.local/share/mocktail /opt/mocktail/data

mkdir /.config
mkdir /.config/mocktail
rm /.config/mocktail/config.yaml
write ${ROOTFS_DIR}/opt/mocktail/config/config.yaml /.config/mocktail/config.yaml
rm /.config/mocktail/fflags.json
write ${ROOTFS_DIR}/opt/mocktail/config/fflags.json /.config/mocktail/fflags.json

mkdir /root/.local
mkdir /root/.local/share
rm /root/.local/share/mocktail
symlink /root/.local/share/mocktail /opt/mocktail/data

mkdir /root/.config
mkdir /root/.config/mocktail
rm /root/.config/mocktail/config.yaml
write ${ROOTFS_DIR}/opt/mocktail/config/config.yaml /root/.config/mocktail/config.yaml
rm /root/.config/mocktail/fflags.json
write ${ROOTFS_DIR}/opt/mocktail/config/fflags.json /root/.config/mocktail/fflags.json

mkdir /home/avory/.local
mkdir /home/avory/.local/share
rm /home/avory/.local/share/mocktail
symlink /home/avory/.local/share/mocktail /opt/mocktail/data
set_inode_field /home/avory/.local uid 1000
set_inode_field /home/avory/.local gid 1000

mkdir /home/avory/.config
mkdir /home/avory/.config/mocktail
rm /home/avory/.config/mocktail/config.yaml
write ${ROOTFS_DIR}/opt/mocktail/config/config.yaml /home/avory/.config/mocktail/config.yaml
set_inode_field /home/avory/.config/mocktail/config.yaml uid 1000
set_inode_field /home/avory/.config/mocktail/config.yaml gid 1000
rm /home/avory/.config/mocktail/fflags.json
write ${ROOTFS_DIR}/opt/mocktail/config/fflags.json /home/avory/.config/mocktail/fflags.json
set_inode_field /home/avory/.config/mocktail/fflags.json uid 1000
set_inode_field /home/avory/.config/mocktail/fflags.json gid 1000
set_inode_field /home/avory/.config uid 1000
set_inode_field /home/avory/.config gid 1000

cd /opt/mocktail
rm roblox
symlink roblox data/${ACTIVE_PAYLOAD_PATH}

cd /usr/bin
rm mocktail
write ${ROOTFS_DIR}/usr/bin/mocktail mocktail
sif mocktail mode 0100755

cd /usr/share/applications
rm mocktail.desktop
write ${ROOTFS_DIR}/usr/share/applications/mocktail.desktop mocktail.desktop
sif mocktail.desktop mode 0100644

mkdir /.cache
sif /.cache mode 040755

mkdir /tmp
sif /tmp mode 040777
EOF

if [ -f "${OPENBOX_MENU}" ]; then
    "${DEBUGFS_BIN}" -w "${PART_IMG}" << EOF >/dev/null 2>&1
cd /etc/xdg/openbox
rm menu.xml
write ${ROOTFS_DIR}/etc/xdg/openbox/menu.xml menu.xml
sif menu.xml mode 0100644
EOF
fi

echo "[*] Re-injecting partition into disk.img..."
dd if="${PART_IMG}" of="${DISK_IMG}" bs=1M seek=1 conv=notrunc status=none
rm -f "${PART_IMG}"

echo ""
echo "=================================================================="
echo "  [SUCCESS] Mocktail AppImage & Roblox payload ready in AvoryOS!"
echo "=================================================================="
echo "  Boot AvoryOS -> open a terminal -> type:  mocktail"
echo ""
echo "  Roblox payload is pre-installed (no in-guest download needed)."
echo "  Configure FFlags in: ~/.config/mocktail/fflags.json"
echo ""
echo "  Note: Audio is DISABLED (SDL_AUDIODRIVER=dummy)"
echo "        Using software OpenGL rendering (llvmpipe)"
echo "=================================================================="
