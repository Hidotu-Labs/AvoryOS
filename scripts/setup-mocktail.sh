#!/usr/bin/env bash
# scripts/setup-mocktail.sh - Downloads and installs the Mocktail AppImage into the AvoryOS disk image
#
# Mocktail 1.0.3 uses the portable usr/share/mocktail-bundle layout; newer
# releases use sharun/AppDir packaging. This script handles both layouts and:
#
#   * downloads the pinned AppImage release (override with MOCKTAIL_VERSION),
#   * installs the runtime under /opt/mocktail-<version>,
#   * keeps the Roblox payload in the shared legacy location /opt/mocktail/data
#     (reused by every mocktail version, so it is never duplicated),
#   * installs the AppImage runtime without binary modifications,
#   * installs launchers:
#       /usr/bin/mocktail            -> new runtime (primary)
#       /usr/bin/mocktail-<version>  -> new runtime
#       /usr/bin/mocktail-<version>  -> selected runtime
#     and removes any stale /usr/bin/mocktail-legacy alias,
#   * injects everything into disk.img.
#
# Usage:
#   ./scripts/setup-mocktail.sh
#   MOCKTAIL_VERSION=1.0.3 ./scripts/setup-mocktail.sh
#   MOCKTAIL_REFRESH_PAYLOAD=0 ./scripts/setup-mocktail.sh # use cached payload offline
#   MOCKTAIL_FORCE_PAYLOAD=1 ./scripts/setup-mocktail.sh   # force payload restage
#
# Prerequisites:
#   1. Run ./scripts/setup-alpine.sh first (installs runtime deps in rootfs)
#   2. Host system needs: wget or curl, sha256sum, dd, debugfs

set -euo pipefail
trap 'status=$?; printf "[!] setup-mocktail.sh failed at line %s: %s (exit %s)\\n" "${LINENO}" "${BASH_COMMAND}" "${status}" >&2' ERR

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DISK_IMG="${ROOT_DIR}/disk.img"
BUILD_DIR="${ROOT_DIR}/build/alpine"
ROOTFS_DIR="${BUILD_DIR}/rootfs"
MOCKTAIL_BUILD_DIR="${BUILD_DIR}/mocktail-build"
HOST_DATA_DIR="${ROOT_DIR}/build/mocktail-host-data"

# ─────────────────────────────────────────────────────────────────────────────
# Release pinning
# ─────────────────────────────────────────────────────────────────────────────
MOCKTAIL_VERSION="${MOCKTAIL_VERSION:-1.0.3}"
MOCKTAIL_REFRESH_PAYLOAD="${MOCKTAIL_REFRESH_PAYLOAD:-1}"
if [ "${MOCKTAIL_VERSION}" = "continuous" ]; then
    APPIMAGE_NAME="mocktail-nightly.AppImage"
elif [ "${MOCKTAIL_VERSION}" = "1.0.3" ]; then
    APPIMAGE_NAME="Mocktail-x86_64.AppImage"
else
    APPIMAGE_NAME="Mocktail-${MOCKTAIL_VERSION}-x86_64.AppImage"
fi
APPIMAGE_URL="https://github.com/komaruworld/mocktail/releases/download/${MOCKTAIL_VERSION}/${APPIMAGE_NAME}"
if [ "${MOCKTAIL_VERSION}" = "continuous" ]; then
    APPIMAGE_SHA256="${APPIMAGE_SHA256:-1253ab0ec719450b14d9956422b60f6d3212b42697559bba88169bdb5e5abe23}"
elif [ "${MOCKTAIL_VERSION}" = "1.0.3" ]; then
    APPIMAGE_SHA256="${APPIMAGE_SHA256:-fe674f9cd5ac870eb94a9ebcfd6f7a824645b6d7275d0b48744c8e81d5629131}"
else
    APPIMAGE_SHA256="${APPIMAGE_SHA256:-07f1c93a00809b436d2f0bb5609f88be20ec3bf7126f253e6e81186d6cb54efe}"
fi

RUNTIME_DIR_NAME="mocktail-${MOCKTAIL_VERSION}"
RUNTIME_INSTALL_DIR="/opt/${RUNTIME_DIR_NAME}"
BUNDLE_ROOT="${ROOTFS_DIR}${RUNTIME_INSTALL_DIR}"
DATA_ROOT="${ROOTFS_DIR}/opt/mocktail/data"

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
# 1. Download the pinned AppImage
# ─────────────────────────────────────────────────────────────────────────────
echo ""
echo "=== [1/5] Downloading Mocktail ${MOCKTAIL_VERSION} ==="

APPIMAGE_FILE="${MOCKTAIL_BUILD_DIR}/mocktail-${MOCKTAIL_VERSION}.AppImage"
NEED_APPIMAGE=1
if [ -f "${APPIMAGE_FILE}" ] && [ -n "${APPIMAGE_SHA256}" ] && \
   [ "$(sha256sum "${APPIMAGE_FILE}" | cut -d' ' -f1)" = "${APPIMAGE_SHA256}" ]; then
    echo "[*] Cached AppImage matches ${APPIMAGE_SHA256:0:12}…, skipping download"
    NEED_APPIMAGE=0
fi

if [ "${NEED_APPIMAGE}" -eq 1 ]; then
    echo "[*] Downloading ${APPIMAGE_NAME}..."
    ${DOWNLOAD_CMD} "${APPIMAGE_FILE}" "${APPIMAGE_URL}"
    chmod +x "${APPIMAGE_FILE}"
    if [ -n "${APPIMAGE_SHA256}" ] && \
       [ "$(sha256sum "${APPIMAGE_FILE}" | cut -d' ' -f1)" != "${APPIMAGE_SHA256}" ]; then
        echo "[!] AppImage sha256 mismatch. Aborting."
        exit 1
    fi
    echo "[+] AppImage downloaded and verified"
fi

# ─────────────────────────────────────────────────────────────────────────────
# 2. Extract and install the runtime
# ─────────────────────────────────────────────────────────────────────────────
echo ""
echo "=== [2/5] Extracting and installing the runtime ==="

EXTRACT_DIR="${MOCKTAIL_BUILD_DIR}/mocktail-extracted-${MOCKTAIL_VERSION}"
rm -rf "${EXTRACT_DIR}"
mkdir -p "${EXTRACT_DIR}"

echo "[*] Extracting AppImage contents..."
(
    cd "${EXTRACT_DIR}"
    "${APPIMAGE_FILE}" --appimage-extract >/dev/null 2>&1 || {
        echo "[!] Failed to extract AppImage."
        exit 1
    }
)

APP_DIR="${EXTRACT_DIR}/squashfs-root"
if [ -f "${APP_DIR}/shared/bin/mocktail" ]; then
    UPDATER_BIN="${APP_DIR}/shared/bin/mocktail_updater"
    RUNTIME_LIBRARY_DIR="${APP_DIR}/lib/mocktail"
    PROJECT_ROOT="${APP_DIR}/share/mocktail"
    COMPATIBILITY_MANIFEST="${PROJECT_ROOT}/metadata/roblox_compatibility.json"
    SIGNING_MANIFEST="${PROJECT_ROOT}/metadata/roblox_signing_certificates.json"
    ABI_REFERENCE="${PROJECT_ROOT}/metadata/roblox_host_abi_reference.json"
    BOOTSTRAP_SOURCES="${PROJECT_ROOT}/metadata/roblox_bootstrap_sources.json"
elif [ -f "${APP_DIR}/usr/share/mocktail-bundle/mocktail/bin/mocktail" ]; then
    LEGACY_ROOT="${APP_DIR}/usr/share/mocktail-bundle/mocktail"
    UPDATER_BIN="${LEGACY_ROOT}/bin/mocktail_updater"
    RUNTIME_LIBRARY_DIR="${LEGACY_ROOT}/lib"
    PROJECT_ROOT="${LEGACY_ROOT}"
    COMPATIBILITY_MANIFEST="${LEGACY_ROOT}/metadata/roblox_compatibility.json"
    SIGNING_MANIFEST="${LEGACY_ROOT}/metadata/roblox_signing_certificates.json"
    ABI_REFERENCE="${LEGACY_ROOT}/metadata/roblox_host_abi_reference.json"
    BOOTSTRAP_SOURCES="${LEGACY_ROOT}/metadata/roblox_bootstrap_sources.json"
else
    echo "[!] Unexpected AppImage layout (Mocktail executable not found)."
    exit 1
fi

echo "[*] Installing runtime to ${RUNTIME_INSTALL_DIR}..."
${SUDO} rm -rf "${BUNDLE_ROOT}"
${SUDO} mkdir -p "${BUNDLE_ROOT}"
${SUDO} cp -a "${APP_DIR}/." "${BUNDLE_ROOT}/"

# WebKitGTK 6.0 in the standalone Mocktail bundle looks up its helper
# processes and injected bundle under the compiled-in namespace path. Normally
# the portable launcher exposes these through bwrap's /usr overlay, but Avory
# skips that namespace because it does not implement mount namespaces. Stage
# links to the bundled files at the same absolute paths so WebKit can spawn
# WebKitNetworkProcess (and its companion processes) without bwrap.
PROJECT_RELATIVE="${PROJECT_ROOT#${APP_DIR}/}"
BUNDLE_PROJECT_ROOT="${BUNDLE_ROOT}/${PROJECT_RELATIVE}"
WEBKIT_ENV_FILE="${BUNDLE_PROJECT_ROOT}/webkit.env"
if [ -f "${WEBKIT_ENV_FILE}" ]; then
    WEBKIT_NAMESPACE_DIR="$(sed -n 's/^MOCKTAIL_WEBKITGTK6_NAMESPACE_DIR=//p' \
        "${WEBKIT_ENV_FILE}" | sed -n '1p')"
    case "${WEBKIT_NAMESPACE_DIR}" in
        /usr/lib/webkitgtk-6.0|/usr/lib64/webkitgtk-6.0|\
        /usr/libexec/webkitgtk-6.0|/usr/lib/x86_64-linux-gnu/webkitgtk-6.0)
            WEBKIT_NAMESPACE_SOURCE="${BUNDLE_PROJECT_ROOT}/namespace${WEBKIT_NAMESPACE_DIR}"
            WEBKIT_SYSTEM_DIR="${ROOTFS_DIR}${WEBKIT_NAMESPACE_DIR}"
            if [ -d "${WEBKIT_NAMESPACE_SOURCE}" ]; then
                ${SUDO} mkdir -p "${WEBKIT_SYSTEM_DIR}"
                for helper in WebKitWebProcess WebKitNetworkProcess WebKitGPUProcess; do
                    if [ -x "${WEBKIT_NAMESPACE_SOURCE}/${helper}" ]; then
                        ${SUDO} ln -sfn \
                            "${RUNTIME_INSTALL_DIR}/${PROJECT_RELATIVE}/namespace${WEBKIT_NAMESPACE_DIR}/${helper}" \
                            "${WEBKIT_SYSTEM_DIR}/${helper}"
                    fi
                done
                if [ -d "${WEBKIT_NAMESPACE_SOURCE}/injected-bundle" ]; then
                    ${SUDO} ln -sfn \
                        "${RUNTIME_INSTALL_DIR}/${PROJECT_RELATIVE}/namespace${WEBKIT_NAMESPACE_DIR}/injected-bundle" \
                        "${WEBKIT_SYSTEM_DIR}/injected-bundle"
                fi
                echo "[+] Exposed bundled WebKitGTK 6.0 helpers at ${WEBKIT_NAMESPACE_DIR}"
            else
                echo "[!] WebKit namespace helper directory is missing: ${WEBKIT_NAMESPACE_SOURCE}"
            fi
            ;;
        *)
            echo "[!] Refusing unexpected WebKit namespace path: ${WEBKIT_NAMESPACE_DIR}"
            ;;
    esac
fi

# Retire the runtimes being replaced. The payload remains at /opt/mocktail/data.
for stale_runtime in "${ROOTFS_DIR}/opt/mocktail-continuous" "${ROOTFS_DIR}/opt/mocktail-1.0.4"; do
    if [ -d "${stale_runtime}" ] && [ "${stale_runtime}" != "${BUNDLE_ROOT}" ]; then
        ${SUDO} rm -rf "${stale_runtime}"
    fi
done
${SUDO} rm -f "${ROOTFS_DIR}/usr/bin/mocktail-continuous" \
    "${ROOTFS_DIR}/usr/bin/mocktail-nightly" \
    "${ROOTFS_DIR}/usr/bin/mocktail104" \
    "${ROOTFS_DIR}/usr/bin/mocktail-1.0.4"
# Do not leave a system GLX link pointing into the removed 1.0.4 bundle.
if [ -L "${ROOTFS_DIR}/usr/lib/libGLX.so.1" ] && \
   [[ "$(readlink "${ROOTFS_DIR}/usr/lib/libGLX.so.1")" == *mocktail-1.0.4* ]]; then
    ${SUDO} rm -f "${ROOTFS_DIR}/usr/lib/libGLX.so.1"
fi

# ─────────────────────────────────────────────────────────────────────────────
# 3. Roblox payload (shared with the legacy install)
# ─────────────────────────────────────────────────────────────────────────────
echo ""
echo "=== [3/5] Preparing the Roblox payload ==="

case "${MOCKTAIL_REFRESH_PAYLOAD}" in
    0|1) ;;
    *) echo "[!] MOCKTAIL_REFRESH_PAYLOAD must be 0 or 1"; exit 1 ;;
esac

NEED_DOWNLOAD=1
if [ -f "${HOST_DATA_DIR}/mocktail/current.json" ]; then
    CURRENT_PAYLOAD_PATH=$(grep -o '"payload_path": "[^"]*"' "${HOST_DATA_DIR}/mocktail/current.json" 2>/dev/null | cut -d'"' -f4 || true)
    if [ -n "${CURRENT_PAYLOAD_PATH}" ] && [ -f "${HOST_DATA_DIR}/mocktail/${CURRENT_PAYLOAD_PATH}/libroblox.so" ]; then
        if [ "${MOCKTAIL_REFRESH_PAYLOAD}" = "1" ]; then
            echo "[*] Cached Roblox payload found; checking for the newest supported x86_64 build..."
        else
            echo "[*] Using cached Roblox payload offline: ${CURRENT_PAYLOAD_PATH}"
            NEED_DOWNLOAD=0
        fi
    fi
fi

if [ "${NEED_DOWNLOAD}" -eq 1 ]; then
    echo "[*] Running mocktail_updater on host to download a supported Roblox payload..."
    if [ ! -x "${UPDATER_BIN}" ]; then
        echo "[!] mocktail_updater binary not found at ${UPDATER_BIN}"
        exit 1
    fi
    env \
        LD_LIBRARY_PATH="${APP_DIR}/lib:${RUNTIME_LIBRARY_DIR}:/lib64:/usr/lib64:${LD_LIBRARY_PATH:-}" \
        MOCKTAIL_RUNTIME_LIBRARY_DIR="${RUNTIME_LIBRARY_DIR}" \
        MOCKTAIL_PROJECT_ROOT="${PROJECT_ROOT}" \
        MOCKTAIL_PACKAGED_COMPATIBILITY_MANIFEST="${COMPATIBILITY_MANIFEST}" \
        MOCKTAIL_UPDATE_COMPATIBILITY_PATH="${COMPATIBILITY_MANIFEST}" \
        MOCKTAIL_UPDATE_SIGNING_TRUST_PATH="${SIGNING_MANIFEST}" \
        MOCKTAIL_UPDATE_HOST_ABI_REFERENCE="${ABI_REFERENCE}" \
        MOCKTAIL_BOOTSTRAP_SOURCES_PATH="${BOOTSTRAP_SOURCES}" \
        MOCKTAIL_PORTABLE_MODE=standalone \
        XDG_DATA_HOME="${HOST_DATA_DIR}" \
        XDG_CACHE_HOME="${HOST_DATA_DIR}/cache" \
        XDG_STATE_HOME="${HOST_DATA_DIR}/state" \
        MOCKTAIL_DATA_ROOT="${HOST_DATA_DIR}/mocktail" \
        MOCKTAIL_CACHE_ROOT="${HOST_DATA_DIR}/cache/mocktail" \
        "${UPDATER_BIN}" update --skip-canary || {
            echo "[!] mocktail_updater failed during host-side download"
            exit 1
        }
    echo "[+] Download and extraction complete on host"
fi

# Do not continue with a stale or incomplete selection after an updater run.
ACTIVE_PAYLOAD_PATH=$(grep -o '"payload_path": "[^"]*"' \
    "${HOST_DATA_DIR}/mocktail/current.json" 2>/dev/null | cut -d'"' -f4 || true)
ACTIVE_PAYLOAD=$(grep -o '"payload_id": "[^"]*"' \
    "${HOST_DATA_DIR}/mocktail/current.json" 2>/dev/null | cut -d'"' -f4 || true)
ACTIVE_PAYLOAD_VERSION=$(grep -o '"version_name": "[^"]*"' \
    "${HOST_DATA_DIR}/mocktail/${ACTIVE_PAYLOAD_PATH}/roblox_payload.json" \
    2>/dev/null | cut -d'"' -f4 || true)
if [[ ! "${ACTIVE_PAYLOAD}" =~ ^[0-9]+-[0-9a-f]{40}$ ]] || \
   [ "${ACTIVE_PAYLOAD_PATH}" != "payloads/${ACTIVE_PAYLOAD}" ] || \
   [ ! -f "${HOST_DATA_DIR}/mocktail/${ACTIVE_PAYLOAD_PATH}/libroblox.so" ] || \
   [ -z "${ACTIVE_PAYLOAD_VERSION}" ]; then
    echo "[!] Updater did not leave a valid supported Roblox payload selected."
    exit 1
fi

# Stage only the selected payload. Inactive updater candidates can be
# read-only (or partially downloaded), and are neither needed by the guest nor
# safe to remove from the updater's cache. The disk image keeps its Android
# userdata independently; only the active payload and manifest are refreshed.
echo "[*] Staging payload in /opt/mocktail/data..."
${SUDO} rm -rf "${DATA_ROOT}"
${SUDO} mkdir -p "${DATA_ROOT}/payloads"
${SUDO} cp -a "${HOST_DATA_DIR}/mocktail/current.json" "${DATA_ROOT}/current.json"
${SUDO} cp -a "${HOST_DATA_DIR}/mocktail/${ACTIVE_PAYLOAD_PATH}" \
    "${DATA_ROOT}/${ACTIVE_PAYLOAD_PATH}"
${SUDO} chmod -R a+rwX "${DATA_ROOT}"
if [ -d "${DATA_ROOT}/android/data/files/appData/LocalStorage" ] && \
   [ ! -e "${DATA_ROOT}/android/data/files/appData/localStorage" ]; then
    ${SUDO} ln -s LocalStorage \
        "${DATA_ROOT}/android/data/files/appData/localStorage"
fi

# Pre-cache the NotoSansCJK fallback font so Roblox never downloads it.
for pdir in "${DATA_ROOT}/payloads"/*; do
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

# Ensure glibc DNS resolver libraries and resolv.conf exist in the rootfs
# (the bundled glibc also ships libnss_*, this keeps the system paths valid).
${SUDO} cp -f "${ROOT_DIR}/toolchain/glibc-sysroot/lib/libnss_dns.so.2" "${ROOTFS_DIR}/lib64/" 2>/dev/null || true
${SUDO} cp -f "${ROOT_DIR}/toolchain/glibc-sysroot/lib/libnss_files.so.2" "${ROOTFS_DIR}/lib64/" 2>/dev/null || true
${SUDO} cp -f "${ROOT_DIR}/toolchain/glibc-sysroot/lib/libnss_dns.so.2" "${ROOTFS_DIR}/lib/" 2>/dev/null || true
${SUDO} cp -f "${ROOT_DIR}/toolchain/glibc-sysroot/lib/libnss_files.so.2" "${ROOTFS_DIR}/lib/" 2>/dev/null || true

# Avory's base runtime is musl. Install the real glibc runtime privately so
# the glibc-only Mocktail 1.0.3 standalone bundle can run. gcompat's ld-linux
# and libc.so.6 stubs are not sufficient for this bundle. Musl programs
# continue to use ld-musl-x86_64.so.1.
GLIBC_SYSROOT="${ROOT_DIR}/toolchain/glibc-sysroot"
if [ ! -s "${GLIBC_SYSROOT}/lib/ld-linux-x86-64.so.2" ] || \
   [ ! -s "${GLIBC_SYSROOT}/lib/libc.so.6" ]; then
    echo "[!] A built x86_64 glibc sysroot is required. Run ./scripts/glibc-toolchain.sh first."
    exit 1
fi
echo "[*] Installing glibc compatibility runtime alongside musl..."
GLIBC_COMPAT_DIR="${ROOTFS_DIR}/opt/avory-glibc/lib"
${SUDO} mkdir -p "${GLIBC_COMPAT_DIR}"
for glibc_library in "${GLIBC_SYSROOT}"/lib/*.so.*; do
    [ -e "${glibc_library}" ] || continue
    ${SUDO} cp -a "${glibc_library}" "${GLIBC_COMPAT_DIR}/"
done
${SUDO} cp -a "${GLIBC_SYSROOT}/lib/ld-linux-x86-64.so.2" "${GLIBC_COMPAT_DIR}/ld-linux-x86-64.so.2"
echo "[*] Staging glibc Mesa/EGL software graphics stack..."
# Avory's /usr/lib contains musl-linked Mesa libraries and cannot serve the
# glibc Mocktail process. Stage the build host's glibc Mesa/GLVND stack and
# software DRI driver privately, including each library's resolved dependency
# closure. This gives SDL/EGL a usable llvmpipe backend without mixing libc.
find_host_library() {
    local soname="$1" path=""
    if command -v ldconfig >/dev/null 2>&1; then
        # Consume all output; an early awk exit trips pipefail via SIGPIPE.
        path="$(ldconfig -p 2>/dev/null | awk -v soname="${soname}" '$1 == soname && !found { path=$NF; found=1 } END { if (found) print path }')"
    fi
    if [ -z "${path}" ] && [ -e "/usr/lib/${soname}" ]; then
        path="/usr/lib/${soname}"
    fi
    printf '%s' "${path}"
}

MESA_HOST_LIBS=()
for host_soname in \
    libdrm.so.2 libgbm.so.1 libexpat.so.1 \
    libEGL.so.1 libEGL_mesa.so.0 libGLdispatch.so.0 \
    libGL.so.1 libGLX.so.0 libGLX_mesa.so.0 libOpenGL.so.0 libGLESv2.so.2; do
    host_library="$(find_host_library "${host_soname}")"
    if [ -n "${host_library}" ] && [ -f "${host_library}" ] && \
       readelf -d "${host_library}" 2>/dev/null | grep -q 'Shared library: \[libc.so.6\]' && \
       ! readelf -d "${host_library}" 2>/dev/null | grep -q 'libc.musl'; then
        ${SUDO} cp -L "${host_library}" "${GLIBC_COMPAT_DIR}/${host_soname}"
        MESA_HOST_LIBS+=("${host_library}")
        echo "[+] Staged ${host_soname} from ${host_library}"
    else
        echo "[!] Missing glibc-linked ${host_soname} on the build host."
    fi
done

HOST_DRI_DIR="${LIBGL_DRIVERS_PATH:-/usr/lib/dri}"
if [ -f "${HOST_DRI_DIR}/swrast_dri.so" ]; then
    ${SUDO} mkdir -p "${ROOTFS_DIR}/opt/avory-glibc/dri"
    ${SUDO} cp -L "${HOST_DRI_DIR}/swrast_dri.so" "${ROOTFS_DIR}/opt/avory-glibc/dri/swrast_dri.so"
    MESA_HOST_LIBS+=("${HOST_DRI_DIR}/swrast_dri.so")
    echo "[+] Staged software DRI driver from ${HOST_DRI_DIR}/swrast_dri.so"
else
    echo "[!] No host swrast_dri.so found at ${HOST_DRI_DIR}; software EGL may not initialize."
fi

if command -v ldd >/dev/null 2>&1; then
    for host_library in "${MESA_HOST_LIBS[@]}"; do
        while IFS= read -r dependency; do
            dependency_soname="$(basename "${dependency}")"
            case "${dependency_soname}" in
                ld-linux-x86-64.so.2|libc.so.6|libm.so.6|libpthread.so.0|libdl.so.2|librt.so.1|libresolv.so.2|libutil.so.1|libanl.so.1|libBrokenLocale.so.1|libnss_*.so.*)
                    continue
                    ;;
            esac
            [ -f "${dependency}" ] || continue
            ${SUDO} cp -L "${dependency}" "${GLIBC_COMPAT_DIR}/${dependency_soname}"
        done < <(ldd "${host_library}" 2>/dev/null | awk '$2 == "=>" && $3 ~ /^\// { print $3 } $1 ~ /^\// { print $1 }' | sort -u)
    done
fi

MESA_VENDOR_DIR="${ROOTFS_DIR}/opt/avory-glibc/share/glvnd/egl_vendor.d"
${SUDO} mkdir -p "${MESA_VENDOR_DIR}"
if [ -f /usr/share/glvnd/egl_vendor.d/50_mesa.json ]; then
    ${SUDO} cp -f /usr/share/glvnd/egl_vendor.d/50_mesa.json "${MESA_VENDOR_DIR}/50_mesa.json"
else
    ${SUDO} tee "${MESA_VENDOR_DIR}/50_mesa.json" > /dev/null <<'MESA_VENDOR_EOF'
{"file_format_version":"1.0.0","ICD":{"library_path":"libEGL_mesa.so.0"}}
MESA_VENDOR_EOF
fi
echo "[*] Installing glibc loader links in the rootfs..."
${SUDO} rm -f "${ROOTFS_DIR}/lib/ld-linux-x86-64.so.2" "${ROOTFS_DIR}/lib/libc.so.6"
${SUDO} ln -s /opt/avory-glibc/lib/ld-linux-x86-64.so.2 "${ROOTFS_DIR}/lib/ld-linux-x86-64.so.2"
${SUDO} ln -s /opt/avory-glibc/lib/libc.so.6 "${ROOTFS_DIR}/lib/libc.so.6"
${SUDO} touch "${ROOTFS_DIR}/etc/avory-glibc-runtime"

# Mocktail asks for ~5 MB guardless pthread stacks, but its Main thread needs
# ~5 MB + a few KB at startup and dies with SIGSEGV in __libc_ns_samename.
# The kernel cannot resize an app-sized stack, so ship a tiny LD_PRELOAD shim
# (glibc-linked) that clamps thread stacks to >= 8 MB + 4 KB guard. Larger
# stacks and explicit mappings pass through untouched.
echo "[*] Building pthread stack-clamp shim..."
GLIBC_CC="${ROOT_DIR}/toolchain/x86_64-linux-glibc/bin/x86_64-buildroot-linux-gnu-gcc"
GLIBC_SYSROOT_DIR="${ROOT_DIR}/toolchain/glibc-sysroot"
if [ ! -x "${GLIBC_CC}" ]; then
    echo "[!] glibc toolchain missing at ${GLIBC_CC}. Run ./scripts/glibc-toolchain.sh first."
    exit 1
fi
"${GLIBC_CC}" -shared -fPIC -O2 -Wall -Wextra -fno-stack-protector \
    --sysroot="${GLIBC_SYSROOT_DIR}" \
    "${ROOT_DIR}/scripts/mocktail-stack-shim.c" \
    -o "${MOCKTAIL_BUILD_DIR}/mocktail-stack-shim.so" -ldl
${SUDO} cp -f "${MOCKTAIL_BUILD_DIR}/mocktail-stack-shim.so" \
    "${ROOTFS_DIR}/opt/avory-glibc/lib/mocktail-stack-shim.so"
echo "[+] Installed stack-clamp shim to /opt/avory-glibc/lib"

# The upstream 1.0.3 preflight equates the shell's libc with the host ABI.
# Avory now has a real glibc loader and libraries for glibc applications, even
# though its base shell remains musl. Teach the bundled check to detect that
# installed runtime, and refresh the bundle's integrity entry for this local
# packaging adaptation.
PORTABLE_LAUNCHER="${BUNDLE_ROOT}/usr/share/mocktail-bundle/mocktail/scripts/portable_launcher.sh"
PORTABLE_CHECKSUMS="${BUNDLE_ROOT}/usr/share/mocktail-bundle/mocktail/metadata/SHA256SUMS.txt"
if [ -f "${PORTABLE_LAUNCHER}" ] && [ -f "${PORTABLE_CHECKSUMS}" ]; then
    if ! grep -Fq 'avory-glibc-runtime' "${PORTABLE_LAUNCHER}"; then
        ${SUDO} sed -i '/DetectHostLibc() {/a\
  if [[ -e /etc/avory-glibc-runtime && -x /lib64/ld-linux-x86-64.so.2 ]]; then\
    printf glibc\
    return 0\
  fi' "${PORTABLE_LAUNCHER}"
        PORTABLE_SHA256="$(sha256sum "${PORTABLE_LAUNCHER}" | cut -d' ' -f1)"
        ${SUDO} sed -i "s|^[0-9a-fA-F]*  ./mocktail/scripts/portable_launcher.sh$|${PORTABLE_SHA256}  ./mocktail/scripts/portable_launcher.sh|" "${PORTABLE_CHECKSUMS}"
    fi
fi
${SUDO} tee "${ROOTFS_DIR}/etc/resolv.conf" > /dev/null << 'RESOLV_EOF'
nameserver 10.0.2.3
nameserver 1.1.1.1
RESOLV_EOF

# ─────────────────────────────────────────────────────────────────────────────
# 4. Launcher scripts, configs and desktop entry
# ─────────────────────────────────────────────────────────────────────────────
echo ""
echo "=== [4/5] Creating launchers and configuration ==="

# Default configuration (used on first launch).
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

# Launcher for the runtime.  Running through AppRun / sharun ensures the
# bundled glibc ld-linux dynamic linker and runtime hooks are used, avoiding
# symbol lookup errors against host/guest musl libc.
${SUDO} mkdir -p "${ROOTFS_DIR}/usr/bin"
${SUDO} tee "${ROOTFS_DIR}/usr/bin/mocktail-${MOCKTAIL_VERSION}" > /dev/null << 'LAUNCHER_EOF'
#!/bin/sh
# Mocktail __VERSION__ launcher for AvoryOS (installed by setup-mocktail.sh).
#
# Runs the AppImage's AppRun entry point with the environment its runtime
# expects, reuses the shared Roblox payload in /opt/mocktail/data, and
# never starts a payload download from the guest.

APPDIR="__RUNTIME_INSTALL_DIR__"
export APPDIR

export MOCKTAIL_DATA_ROOT="${MOCKTAIL_DATA_ROOT:-/opt/mocktail/data}"
export MOCKTAIL_SKIP_UPDATE_CHECK=1
export MOCKTAIL_PORTABLE_MODE=standalone
export MOCKTAIL_DISABLE_FAILURE_DIALOG=1
# Avory does not implement mount namespaces; avoid the bundle's bwrap wrapper,
# which otherwise tries mount(NULL, "/", MS_SLAVE|MS_REC) and aborts.
export MOCKTAIL_SKIP_NAMESPACE_CHECK=1
# Keep the glibc app isolated from Avory's musl-linked /usr/lib libraries.
# Required host-side compatibility libraries are staged into this private dir.
export LD_LIBRARY_PATH="/opt/avory-glibc/lib"
# Widen mocktail's undersized pthread stacks (5 MB guardless overflows in its
# Main thread). The shim only raises stacks below 8 MB / guards below 4 KB.
# Disable with MOCKTAIL_STACK_SHIM_OFF=1.
export LD_PRELOAD="/opt/avory-glibc/lib/mocktail-stack-shim.so${LD_PRELOAD:+:$LD_PRELOAD}"

# Roblox currently spells this directory `localStorage`, while the staged
# Android data tree from the payload uses `LocalStorage`. Linux paths are
# case-sensitive, so the lowercase path otherwise fails with ENOENT during
# app startup. Keep both spellings pointed at the same writable data.
ANDROID_APPDATA="${MOCKTAIL_DATA_ROOT}/android/data/files/appData"
if [ -d "${ANDROID_APPDATA}/LocalStorage" ] && [ ! -e "${ANDROID_APPDATA}/localStorage" ]; then
    ln -s LocalStorage "${ANDROID_APPDATA}/localStorage" 2>/dev/null || true
else
    mkdir -p "${ANDROID_APPDATA}/localStorage"
fi

# AvoryOS: a killed Mocktail run can leave its external-launch socket behind.
ENDPOINT_DIR="/tmp/mocktail-$(id -u 2>/dev/null || echo 0)"
if ! pgrep -f "mocktail" >/dev/null 2>&1; then
    rm -rf "${ENDPOINT_DIR}" 2>/dev/null || true
    find "${MOCKTAIL_DATA_ROOT}" -name "*.lock" -delete 2>/dev/null || true
fi

# Windowing and software OpenGL.
export DISPLAY="${DISPLAY:-:0}"
export SDL_VIDEODRIVER=x11
# Let SDL3 select the real system audio backend (ALSA/PulseAudio/PipeWire).
# Clear inherited overrides so Mocktail cannot accidentally get the dummy
# backend from a shell or desktop launcher environment.
unset SDL_AUDIODRIVER
export LIBGL_ALWAYS_SOFTWARE=1
export GALLIUM_DRIVER=llvmpipe
export MESA_LOADER_DRIVER_OVERRIDE=swrast
export LIBGL_DRIVERS_PATH=/opt/avory-glibc/dri
export __EGL_VENDOR_LIBRARY_FILENAMES=/opt/avory-glibc/share/glvnd/egl_vendor.d/50_mesa.json
export MESA_GL_VERSION_OVERRIDE=3.3
export MESA_GLSL_VERSION_OVERRIDE=330
export WEBKIT_DISABLE_COMPOSITING_MODE=1
export XCURSOR_THEME=Adwaita
export XCURSOR_SIZE=24
export XCURSOR_PATH=/usr/share/icons:/usr/share/pixmaps
# On the guest X11 path SDL can enter relative mode while Roblox is still on
# its menu. Keep the SDL system cursor visible in that mode and use SDL's
# default system cursor instead of inheriting a missing/empty cursor shape.
export SDL_MOUSE_RELATIVE_CURSOR_VISIBLE=1
export SDL_MOUSE_DEFAULT_SYSTEM_CURSOR=0
# Do not inherit verbose SDL diagnostics from the shell or desktop session.
unset SDL_LOGGING SDL_EVENT_LOGGING

export XDG_CONFIG_HOME="${XDG_CONFIG_HOME:-${HOME:-/root}/.config}"
export XDG_DATA_HOME="${XDG_DATA_HOME:-${HOME:-/root}/.local/share}"
export XDG_CACHE_HOME="${XDG_CACHE_HOME:-${HOME:-/root}/.cache}"
export XDG_STATE_HOME="${XDG_STATE_HOME:-${HOME:-/root}/.local/state}"

# Default to the software OpenGL backend unless the user picked one.
has_graphics=0
for arg in "$@"; do
    case "$arg" in
        --graphics*) has_graphics=1 ;;
    esac
done
if [ "$has_graphics" -eq 0 ]; then
    set -- --graphics opengl "$@"
fi

if [ -x "$APPDIR/AppRun" ]; then
    exec "$APPDIR/AppRun" "$@"
elif [ -x "$APPDIR/bin/mocktail" ]; then
    exec "$APPDIR/bin/mocktail" "$@"
elif [ -x "$APPDIR/lib/ld-linux-x86-64.so.2" ] && [ -x "$APPDIR/shared/bin/mocktail" ]; then
    exec "$APPDIR/lib/ld-linux-x86-64.so.2" --library-path "$APPDIR/lib:$APPDIR/lib/mocktail" "$APPDIR/shared/bin/mocktail" "$@"
else
    exec "$APPDIR/shared/bin/mocktail" "$@"
fi
LAUNCHER_EOF

# Substitute the version and install path, then add the aliases.
${SUDO} sed -i \
    -e "s|__VERSION__|${MOCKTAIL_VERSION}|g" \
    -e "s|__RUNTIME_INSTALL_DIR__|${RUNTIME_INSTALL_DIR}|g" \
    "${ROOTFS_DIR}/usr/bin/mocktail-${MOCKTAIL_VERSION}"
${SUDO} chmod +x "${ROOTFS_DIR}/usr/bin/mocktail-${MOCKTAIL_VERSION}"
if [ "${MOCKTAIL_VERSION}" = "continuous" ]; then
    ${SUDO} cp -f "${ROOTFS_DIR}/usr/bin/mocktail-${MOCKTAIL_VERSION}" "${ROOTFS_DIR}/usr/bin/mocktail-nightly"
fi
${SUDO} cp -f "${ROOTFS_DIR}/usr/bin/mocktail-${MOCKTAIL_VERSION}" "${ROOTFS_DIR}/usr/bin/mocktail"
echo "[+] Created /usr/bin/mocktail (${MOCKTAIL_VERSION})"
if [ "${MOCKTAIL_VERSION}" = "continuous" ]; then
    echo "[+] Created /usr/bin/mocktail-nightly alias"
fi

# Desktop entry (kept named mocktail so existing menus keep working).
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

# ─────────────────────────────────────────────────────────────────────────────
# 5. Inject into disk.img
# ─────────────────────────────────────────────────────────────────────────────
echo ""
echo "=== [5/5] Injecting into disk.img ==="

PART_IMG="${MOCKTAIL_BUILD_DIR}/part_mocktail.img"
dd if="${DISK_IMG}" of="${PART_IMG}" bs=1M skip=1 status=none

DEBUGFS_BIN="debugfs"
[ -x /run/host/usr/bin/debugfs ] && DEBUGFS_BIN=/run/host/usr/bin/debugfs

# Runtime tree (always rewritten).
"${ROOT_DIR}/scripts/populate-ext2-dir.sh" "${PART_IMG}" "${BUNDLE_ROOT}" "${RUNTIME_INSTALL_DIR}"
"${ROOT_DIR}/scripts/populate-ext2-dir.sh" "${PART_IMG}" \
    "${ROOTFS_DIR}/opt/avory-glibc" /opt/avory-glibc

# Refresh only the selected payload and its manifest. Preserve Android data,
# cookies, logs, and local storage already inside /opt/mocktail/data.
IMAGE_PAYLOAD_ID=$("${DEBUGFS_BIN}" -R "cat /opt/mocktail/data/current.json" \
    "${PART_IMG}" 2>/dev/null | grep -o '"payload_id": "[^"]*"' | cut -d'"' -f4 || true)
if [ "${MOCKTAIL_FORCE_PAYLOAD:-0}" = "1" ] || \
   [ "${IMAGE_PAYLOAD_ID}" != "${ACTIVE_PAYLOAD}" ]; then
    echo "[*] Installing supported Roblox ${ACTIVE_PAYLOAD_VERSION} (${ACTIVE_PAYLOAD}) into disk.img"
    "${ROOT_DIR}/scripts/populate-ext2-dir.sh" "${PART_IMG}" \
        "${DATA_ROOT}/${ACTIVE_PAYLOAD_PATH}" "/opt/mocktail/data/${ACTIVE_PAYLOAD_PATH}"

    if "${DEBUGFS_BIN}" -R "stat /opt/mocktail/data/current.json" "${PART_IMG}" >/dev/null 2>&1; then
        "${DEBUGFS_BIN}" -w -R "rm /opt/mocktail/data/current.json" \
            "${PART_IMG}" >/dev/null 2>&1 || {
            echo "[!] Could not replace the Roblox payload manifest in disk.img"
            exit 1
        }
    fi
    "${DEBUGFS_BIN}" -w -R \
        "write ${DATA_ROOT}/current.json /opt/mocktail/data/current.json" \
        "${PART_IMG}" >/dev/null 2>&1 || {
        echo "[!] Could not install the Roblox payload manifest in disk.img"
        exit 1
    }
    "${DEBUGFS_BIN}" -w -R "sif /opt/mocktail/data/current.json mode 0100644" \
        "${PART_IMG}" >/dev/null 2>&1 || true
else
    echo "[*] Roblox payload ${ACTIVE_PAYLOAD} is already in disk.img"
fi

# Drop runtimes from previous installs and their entry points. Only the shared payload
# (/opt/mocktail/data) and configs survive from the legacy layout.  debugfs'
# rm_rf command is missing from some e2fsprogs builds, so this is best-effort:
# the small files are unlinked in a separate command file (missing files must
# not fail the main batch) and a failed tree removal only leaves disk usage.
"${DEBUGFS_BIN}" -w -R "rm_rf /opt/mocktail/usr" "${PART_IMG}" >/dev/null 2>&1 || true
"${DEBUGFS_BIN}" -w -R "rm_rf /opt/mocktail-continuous" "${PART_IMG}" >/dev/null 2>&1 || true
"${DEBUGFS_BIN}" -w -R "rm_rf /opt/mocktail-1.0.4" "${PART_IMG}" >/dev/null 2>&1 || true
LEGACY_CMDS="$(mktemp)"
{
    echo "cd /"
    echo "rm /opt/mocktail/AppRun"
    echo "rm /opt/mocktail/space.bigrat.mocktail.desktop"
    echo "rm /opt/mocktail/space.bigrat.mocktail.svg"
    echo "rm /opt/mocktail/roblox"
    echo "rm /usr/bin/mocktail.desktop"
    echo "rm /usr/bin/mocktail-continuous"
    echo "rm /usr/bin/mocktail-nightly"
    echo "rm /usr/bin/mocktail104"
    echo "rm /usr/bin/mocktail-1.0.4"
    echo "rm /usr/lib/libGLX.so.1"
    echo "rm /lib/ld-linux-x86-64.so.2"
    echo "symlink /lib/ld-linux-x86-64.so.2 /opt/avory-glibc/lib/ld-linux-x86-64.so.2"
    echo "rm /lib/libc.so.6"
    echo "symlink /lib/libc.so.6 /opt/avory-glibc/lib/libc.so.6"
} > "${LEGACY_CMDS}"
"${DEBUGFS_BIN}" -w -f "${LEGACY_CMDS}" "${PART_IMG}" >/dev/null 2>&1 || true
rm -f "${LEGACY_CMDS}"
if "${DEBUGFS_BIN}" -R "ls -l /opt/mocktail" "${PART_IMG}" 2>/dev/null | grep -q " usr$"; then
    echo "[!] This debugfs has no rm_rf; the legacy runtime is still at /opt/mocktail/usr."
    echo "    Remove it from inside AvoryOS with:  rm -rf /opt/mocktail/usr"
fi

# Launchers, configs and XDG symlinks.
{
    echo "cd /usr/bin"
    echo "rm mocktail"
    echo "write ${ROOTFS_DIR}/usr/bin/mocktail mocktail"
    echo "sif mocktail mode 0100755"
    echo "rm mocktail-continuous"
    echo "rm mocktail-nightly"
    echo "rm mocktail104"
    echo "rm mocktail104"
    echo "rm mocktail-${MOCKTAIL_VERSION}"
    echo "write ${ROOTFS_DIR}/usr/bin/mocktail-${MOCKTAIL_VERSION} mocktail-${MOCKTAIL_VERSION}"
    echo "sif mocktail-${MOCKTAIL_VERSION} mode 0100755"
    # Drop stale launcher aliases if an earlier setup left them.
    echo "rm mocktail-legacy"
    echo "rm mocktail.bin"

    echo "mkdir /opt/mocktail"
    echo "mkdir /opt/mocktail/config"
    echo "rm /opt/mocktail/config/config.yaml"
    echo "write ${ROOTFS_DIR}/opt/mocktail/config/config.yaml /opt/mocktail/config/config.yaml"
    echo "sif /opt/mocktail/config/config.yaml mode 0100644"
    echo "rm /opt/mocktail/config/fflags.json"
    echo "write ${ROOTFS_DIR}/opt/mocktail/config/fflags.json /opt/mocktail/config/fflags.json"
    echo "sif /opt/mocktail/config/fflags.json mode 0100644"

    echo "cd /"

    # Desktop entry (name kept as mocktail so existing menus keep working).
    echo "mkdir /usr/share/applications"
    echo "rm /usr/share/applications/mocktail.desktop"
    echo "write ${ROOTFS_DIR}/usr/share/applications/mocktail.desktop /usr/share/applications/mocktail.desktop"
    echo "sif /usr/share/applications/mocktail.desktop mode 0100644"

    echo "rm /.local/share/mocktail"
    echo "mkdir /.local"
    echo "mkdir /.local/share"
    echo "mkdir /.local/state"
    echo "mkdir /.local/state/mocktail"
    echo "symlink /.local/share/mocktail /opt/mocktail/data"

    echo "mkdir /.config"
    echo "mkdir /.config/mocktail"
    echo "rm /.config/mocktail/config.yaml"
    echo "write ${ROOTFS_DIR}/opt/mocktail/config/config.yaml /.config/mocktail/config.yaml"
    echo "rm /.config/mocktail/fflags.json"
    echo "write ${ROOTFS_DIR}/opt/mocktail/config/fflags.json /.config/mocktail/fflags.json"

    echo "mkdir /root/.local"
    echo "mkdir /root/.local/share"
    # Mocktail stores window geometry below XDG_STATE_HOME (by default
    # ~/.local/state/mocktail). Pre-create the directory on the image so the
    # first window-state save does not depend on runtime mkdir support.
    echo "mkdir /root/.local/state"
    echo "mkdir /root/.local/state/mocktail"
    echo "rm /root/.local/share/mocktail"
    echo "symlink /root/.local/share/mocktail /opt/mocktail/data"

    echo "mkdir /root/.config"
    echo "mkdir /root/.config/mocktail"
    echo "rm /root/.config/mocktail/config.yaml"
    echo "write ${ROOTFS_DIR}/opt/mocktail/config/config.yaml /root/.config/mocktail/config.yaml"
    echo "rm /root/.config/mocktail/fflags.json"
    echo "write ${ROOTFS_DIR}/opt/mocktail/config/fflags.json /root/.config/mocktail/fflags.json"

    echo "mkdir /home/avory/.local"
    echo "mkdir /home/avory/.local/share"
    echo "mkdir /home/avory/.local/state"
    echo "mkdir /home/avory/.local/state/mocktail"
    echo "rm /home/avory/.local/share/mocktail"
    echo "symlink /home/avory/.local/share/mocktail /opt/mocktail/data"
    echo "set_inode_field /home/avory/.local uid 1000"
    echo "set_inode_field /home/avory/.local gid 1000"
    echo "set_inode_field /home/avory/.local/state uid 1000"
    echo "set_inode_field /home/avory/.local/state gid 1000"
    echo "set_inode_field /home/avory/.local/state/mocktail uid 1000"
    echo "set_inode_field /home/avory/.local/state/mocktail gid 1000"

    echo "mkdir /home/avory/.config"
    echo "mkdir /home/avory/.config/mocktail"
    echo "rm /home/avory/.config/mocktail/config.yaml"
    echo "write ${ROOTFS_DIR}/opt/mocktail/config/config.yaml /home/avory/.config/mocktail/config.yaml"
    echo "set_inode_field /home/avory/.config/mocktail/config.yaml uid 1000"
    echo "set_inode_field /home/avory/.config/mocktail/config.yaml gid 1000"
    echo "rm /home/avory/.config/mocktail/fflags.json"
    echo "write ${ROOTFS_DIR}/opt/mocktail/config/fflags.json /home/avory/.config/mocktail/fflags.json"
    echo "set_inode_field /home/avory/.config/mocktail/fflags.json uid 1000"
    echo "set_inode_field /home/avory/.config/mocktail/fflags.json gid 1000"
    echo "set_inode_field /home/avory/.config uid 1000"
    echo "set_inode_field /home/avory/.config gid 1000"
    echo "rm /home/avory/.xinitrc"
    echo "write ${ROOTFS_DIR}/home/avory/.xinitrc /home/avory/.xinitrc"
    echo "sif /home/avory/.xinitrc mode 0100755"
    echo "set_inode_field /home/avory/.xinitrc uid 1000"
    echo "set_inode_field /home/avory/.xinitrc gid 1000"

    echo "mkdir /.cache"
    echo "sif /.cache mode 040755"

    echo "mkdir /tmp"
    echo "sif /tmp mode 040777"
} > "${MOCKTAIL_BUILD_DIR}/mocktail-debugfs.cmds"

"${DEBUGFS_BIN}" -w -f "${MOCKTAIL_BUILD_DIR}/mocktail-debugfs.cmds" "${PART_IMG}" >/dev/null 2>&1 || {
    echo "[!] debugfs launcher/config injection failed"
    rm -f "${MOCKTAIL_BUILD_DIR}/mocktail-debugfs.cmds"
    exit 1
}
rm -f "${MOCKTAIL_BUILD_DIR}/mocktail-debugfs.cmds"

if [ -f "${OPENBOX_MENU}" ]; then
    "${DEBUGFS_BIN}" -w "${PART_IMG}" >/dev/null 2>&1 << EOF
cd /etc/xdg/openbox
rm menu.xml
write ${OPENBOX_MENU} menu.xml
sif menu.xml mode 0100644
EOF
fi

echo "[*] Re-injecting partition into disk.img..."
dd if="${PART_IMG}" of="${DISK_IMG}" bs=1M seek=1 conv=notrunc status=none
rm -f "${PART_IMG}"

echo ""
echo "=================================================================="
echo "  [SUCCESS] Mocktail ${MOCKTAIL_VERSION} installed in AvoryOS!"
echo "=================================================================="
echo "  Boot AvoryOS -> open a terminal -> type:  mocktail"
echo ""
echo "  Runtime:  ${RUNTIME_INSTALL_DIR}"
echo "  Payload:  /opt/mocktail/data (shared, version-independent)"
echo "  Roblox:   ${ACTIVE_PAYLOAD_VERSION} (newest exact-supported x86_64 build)"
echo "  Config:   ~/.config/mocktail/{config.yaml,fflags.json}"
echo ""
echo "  Audio: SDL3 automatic system backend selection"
echo "        Using software OpenGL rendering (llvmpipe)"
echo "=================================================================="
