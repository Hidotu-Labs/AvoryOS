#!/usr/bin/env bash
# Provision the AvoryOS desktop rootfs with Axon, independent of the host distro.
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${ROOT_DIR}/build/arch"
ROOTFS_DIR="${BUILD_DIR}/rootfs"

source "${ROOT_DIR}/scripts/axon-common.sh"

require_command() {
    if ! command -v "$1" >/dev/null 2>&1; then
        echo "[!] Required host command not found: $1" >&2
        return 1
    fi
}

for tool in bash python3 make unshare mount chroot tar readelf sha256sum curl; do
    require_command "${tool}"
done

if [ "${AXON_BIN:-}" = "${AXON_SOURCE_DIR}/axon" ]; then
    unset AXON_BIN
fi
ensure_axon
mkdir -p "${BUILD_DIR}" "${ROOTFS_DIR}"

# Use and build the latest managed Axon checkout from main. AvoryOS provisions
# the target root with Axon's supported --root/get interface.
if [ "${AXON_BIN}" != "${AXON_SOURCE_DIR}/axon" ]; then
    echo "[!] Rootfs setup requires the AvoryOS-managed Axon checkout at ${AXON_SOURCE_DIR}." >&2
    echo "    Clear AXON_BIN or point AXON_SOURCE_DIR at a checkout inside this repository." >&2
    exit 1
fi
make -C "${AXON_SOURCE_DIR}" axon

if [ "${1:-}" = "--nouveau-userspace" ]; then
    if [ "$#" -ne 1 ]; then
        echo "Usage: $0 [--nouveau-userspace]" >&2
        exit 2
    fi

    axon_rootfs_sync "${ROOTFS_DIR}"
    axon_rootfs_command "${ROOTFS_DIR}" get --needed --noconfirm --keep-cache \
        --assume-installed ntsync-autoload \
        mesa vulkan-nouveau vulkan-radeon vulkan-swrast \
        vulkan-mesa-implicit-layers vulkan-icd-loader vulkan-tools mesa-utils
    install -d "${ROOTFS_DIR}/usr/share/avory-xorg" "${ROOTFS_DIR}/usr/bin"
    install -m 0644 "${ROOT_DIR}/config/xorg/10-nouveau.conf" \
        "${ROOTFS_DIR}/usr/share/avory-xorg/10-modesetting-nouveau.conf"
    install -m 0755 "${ROOT_DIR}/config/xorg/avory-nouveau-xorg.sh" \
        "${ROOTFS_DIR}/usr/bin/avory-nouveau-xorg"
    install -m 0755 "${ROOT_DIR}/config/xorg/avory-xorg-profile.sh" \
        "${ROOTFS_DIR}/usr/bin/avory-xorg-profile"
    install -d "${ROOTFS_DIR}/etc/X11/xinit/xinitrc.d" "${ROOTFS_DIR}/etc/profile.d"
    install -m 0755 "${ROOT_DIR}/config/xorg/44-nouveau-env.sh" \
        "${ROOTFS_DIR}/etc/X11/xinit/xinitrc.d/44-nouveau-env.sh"
    install -m 0644 "${ROOT_DIR}/config/xorg/44-nouveau-env.sh" \
        "${ROOTFS_DIR}/etc/profile.d/avory-nouveau.sh"
    install -d "${ROOTFS_DIR}/etc/sddm.conf.d"
    install -m 0644 "${ROOT_DIR}/config/xorg/20-output.conf" \
        "${ROOTFS_DIR}/etc/sddm.conf.d/20-output.conf"
    install -m 0644 "${ROOT_DIR}/config/sddm/10-theme.conf" \
        "${ROOTFS_DIR}/etc/sddm.conf.d/10-theme.conf"
    exit 0
elif [ "$#" -ne 0 ]; then
    echo "Usage: $0 [--nouveau-userspace]" >&2
    exit 2
fi

check_glibc_244() {
    local glibc_lib="${ROOTFS_DIR}/usr/lib/libc.so.6"
    if [ ! -e "${glibc_lib}" ]; then
        echo "[!] Axon did not install ${glibc_lib} into the target root." >&2
        return 1
    fi
    python3 - "${glibc_lib}" <<'PY'
from pathlib import Path
import re
import sys

data = Path(sys.argv[1]).read_bytes()
match = re.search(rb"stable release version ([0-9]+(?:\.[0-9]+)*)", data)
if not match or match.group(1) != b"2.44":
    version = match.group(1).decode() if match else "unknown"
    print(f"[!] Expected glibc 2.44 in the target root, found {version}.", file=sys.stderr)
    raise SystemExit(1)
PY
}

# Bootstrap the empty target with the current Axon CLI, then install the
# compiler's libc dependency before linking AvoryOS's login program.
axon_rootfs_sync "${ROOTFS_DIR}"
axon_rootfs_bootstrap "${ROOTFS_DIR}"
check_glibc_244
axon_rootfs_command "${ROOTFS_DIR}" get --needed --noconfirm --keep-cache libxcrypt

GLIBC_CC="${ROOT_DIR}/toolchain/x86_64-linux-glibc/bin/x86_64-linux-gcc"
if [ ! -x "${GLIBC_CC}" ]; then
    echo "[*] Setting up the prebuilt GCC cross-compiler..."
    sh "${ROOT_DIR}/scripts/glibc-toolchain.sh"
fi

"${GLIBC_CC}" -O2 -Wall -Wextra -fno-stack-protector \
    --sysroot="${ROOTFS_DIR}" \
    -Wl,-T,"${ROOT_DIR}/config/userland-link.ld" \
    "${ROOT_DIR}/userland/avory-login.c" -lcrypt \
    -o "${ROOT_DIR}/userland/avory-login.elf"

axon_rootfs_install_list "${ROOTFS_DIR}" \
    "${ROOT_DIR}/scripts/rootfs-packages.txt" \
    --assume-installed ntsync-autoload
check_glibc_244

# The kernel starts the system through /sbin/init. Arch uses usrmerge, so
# /sbin points into /usr/bin; install BusyBox's init applet link in the rootfs
# before image population instead of patching the ext4 image afterward.
if [ ! -x "${ROOTFS_DIR}/usr/bin/busybox" ]; then
    echo "[!] BusyBox is missing from the target root; cannot install /sbin/init." >&2
    exit 1
fi
ln -sfn /usr/bin/busybox "${ROOTFS_DIR}/sbin/init"

# WirePlumber comes from Arch and can need a newer Lua ABI than Axon's native
# interpreter. Supply its versioned runtime alongside the native one, using
# the package and checksum from the same repository index as the desktop.
LUA_MODULE="${ROOTFS_DIR}/usr/lib/wireplumber-0.5/libwireplumber-module-lua-scripting.so"
LUA_SONAME="$(LC_ALL=C readelf -d "${LUA_MODULE}" | sed -n 's/.*(NEEDED).*\[\(liblua\.so\.[0-9.]*\)\].*/\1/p')"
if [[ ! "${LUA_SONAME}" =~ ^liblua\.so\.[0-9]+\.[0-9]+$ ]]; then
    echo "[!] Cannot identify WirePlumber's required Lua ABI." >&2
    exit 1
fi
if [ ! -e "${ROOTFS_DIR}/usr/lib/${LUA_SONAME}" ] ||
   ! LC_ALL=C readelf -d "${ROOTFS_DIR}/usr/lib/${LUA_SONAME}" 2>/dev/null | grep -Fq "[${LUA_SONAME}]"; then
    LUA_DB="${ROOTFS_DIR}/var/lib/axon/sync/extra.db"
    LUA_DESC="$(tar -tf "${LUA_DB}" | awk '/^lua-[0-9][^/]*\/desc$/ { print }')"
    LUA_METADATA="$(tar -xOf "${LUA_DB}" "${LUA_DESC}")"
    LUA_FILENAME="$(printf '%s\n' "${LUA_METADATA}" | awk '$0 == "%FILENAME%" { getline; print; exit }')"
    LUA_SHA256="$(printf '%s\n' "${LUA_METADATA}" | awk '$0 == "%SHA256SUM%" { getline; print; exit }')"
    [[ "${LUA_FILENAME}" =~ ^lua-[a-zA-Z0-9.+_-]+\.pkg\.tar\.zst$ && "${LUA_SHA256}" =~ ^[0-9a-f]{64}$ ]] || {
        echo "[!] Invalid Arch Lua package metadata." >&2
        exit 1
    }
    LUA_ARCHIVE="${ROOTFS_DIR}/var/cache/axon/pkg/${LUA_FILENAME}"
    if [ ! -f "${LUA_ARCHIVE}" ]; then
        mkdir -p "$(dirname "${LUA_ARCHIVE}")"
        for lua_mirror in fastly.mirror.pkgbuild.com geo.mirror.pkgbuild.com; do
            if curl --fail --location --max-time 60 \
                "https://${lua_mirror}/extra/os/x86_64/${LUA_FILENAME}" -o "${LUA_ARCHIVE}.tmp"; then
                mv "${LUA_ARCHIVE}.tmp" "${LUA_ARCHIVE}"
                break
            fi
        done
    fi
    printf '%s  %s\n' "${LUA_SHA256}" "${LUA_ARCHIVE}" | sha256sum -c -
    mapfile -t LUA_MEMBERS < <(tar -tf "${LUA_ARCHIVE}" | awk -v soname="${LUA_SONAME}" \
        'index($0, "usr/lib/" soname ".") == 1 && $0 ~ /[0-9]$/ { print }')
    [ "${#LUA_MEMBERS[@]}" -eq 1 ] || {
        echo "[!] Arch Lua package does not supply ${LUA_SONAME}." >&2
        exit 1
    }
    LUA_TEMP="$(mktemp "${ROOTFS_DIR}/usr/lib/.lua-runtime.XXXXXX")"
    tar -xOf "${LUA_ARCHIVE}" "${LUA_MEMBERS[0]}" > "${LUA_TEMP}"
    LC_ALL=C readelf -d "${LUA_TEMP}" | grep -Fq "[${LUA_SONAME}]"
    chmod 0755 "${LUA_TEMP}"
    mv "${LUA_TEMP}" "${ROOTFS_DIR}/${LUA_MEMBERS[0]}"
    ln -sfn "$(basename "${LUA_MEMBERS[0]}")" "${ROOTFS_DIR}/usr/lib/${LUA_SONAME}"
fi

# Profiles are selected at boot from the negotiated DRM identity.
install -d "${ROOTFS_DIR}/usr/share/avory-xorg" "${ROOTFS_DIR}/usr/bin"
for profile in native virgl; do
    install -m 0644 "${ROOT_DIR}/config/xorg/10-${profile}.conf" \
        "${ROOTFS_DIR}/usr/share/avory-xorg/10-modesetting-${profile}.conf"
done
install -m 0755 "${ROOT_DIR}/config/xorg/avory-xorg-profile.sh" \
    "${ROOTFS_DIR}/usr/bin/avory-xorg-profile"

# Keep the AvoryOS VM-specific WirePlumber policy alongside the desktop
# profile's package and service setup.
WIREPLUMBER_CONF_DIR="${ROOTFS_DIR}/etc/wireplumber/wireplumber.conf.d"
install -d "${WIREPLUMBER_CONF_DIR}"
cat > "${WIREPLUMBER_CONF_DIR}/10-avoryos-vm.conf" <<'EOF'
wireplumber.profiles = {
  main = {
    hardware.video-capture = disabled
    hardware.bluetooth = disabled
    monitor.alsa-midi = disabled
    monitor.bluez-midi = disabled
    support.portal-permissionstore = disabled
    support.reserve-device = disabled
    support.logind = disabled
    monitor.bluez.seat-monitoring = disabled
  }
}
EOF
install -m 0644 "${ROOT_DIR}/config/wireplumber/20-avoryos-hda.conf" \
    "${WIREPLUMBER_CONF_DIR}/20-avoryos-hda.conf"

# Keep boot service startup in the versioned init script.
install -D -m 0755 "${ROOT_DIR}/config/init/rcS" "${ROOTFS_DIR}/etc/init.d/rcS"
install -D -m 0644 "${ROOT_DIR}/config/init/inittab" "${ROOTFS_DIR}/etc/inittab"
install -D -m 0644 "${ROOT_DIR}/config/sddm/10-theme.conf" \
    "${ROOTFS_DIR}/etc/sddm.conf.d/10-theme.conf"

# Weston needs an explicit production display rather than VKMS heuristics.
install -m 0755 "${ROOT_DIR}/config/weston/avory-weston-session.sh" "${ROOTFS_DIR}/usr/bin/avory-weston-session"
install -m 0644 "${ROOT_DIR}/config/weston/weston.desktop" "${ROOTFS_DIR}/usr/share/wayland-sessions/weston.desktop"
bash "${ROOT_DIR}/scripts/configure-pacman.sh" "${ROOTFS_DIR}"
