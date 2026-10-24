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

for tool in bash python3 make unshare mount chroot; do
    require_command "${tool}"
done

ensure_axon
mkdir -p "${BUILD_DIR}" "${ROOTFS_DIR}"

# Use the managed Axon checkout so AvoryOS can configure mirror fallbacks,
# and rebuild the binary from the checked out source.
if [ "${AXON_BIN}" != "${AXON_SOURCE_DIR}/axon" ]; then
    echo "[!] Rootfs setup requires the AvoryOS-managed Axon checkout at ${AXON_SOURCE_DIR}." >&2
    echo "    Clear AXON_BIN or point AXON_SOURCE_DIR at a checkout inside this repository." >&2
    exit 1
fi
make -C "${AXON_SOURCE_DIR}" axon

# Axon rootfs currently writes a one-server mirrorlist internally. Patch its
# generated runtime to retain official fallbacks if the first mirror is stale.
AXON_RUNTIME=""
for candidate in \
    "${AXON_SOURCE_DIR}/libexec/axon-runtime.py" \
    "$(dirname "${AXON_BIN}")/libexec/axon-runtime.py" \
    "$(dirname "${AXON_BIN}")/../libexec/axon/axon-runtime.py"; do
    if [ -f "${candidate}" ]; then
        AXON_RUNTIME="${candidate}"
        break
    fi
done
if [ -z "${AXON_RUNTIME}" ]; then
    echo "[!] Could not locate Axon's rootfs runtime to configure mirror fallbacks." >&2
    exit 1
fi
python3 "${ROOT_DIR}/scripts/configure-axon-mirrors.py" \
    "${AXON_RUNTIME}" "${ROOT_DIR}"

if [ "${1:-}" = "--nouveau-userspace" ]; then
    if [ "$#" -ne 1 ]; then
        echo "Usage: $0 [--nouveau-userspace]" >&2
        exit 2
    fi

    "${AXON_BIN}" get --root "${ROOTFS_DIR}" --needed --noconfirm --keep-cache \
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
    exit 0
elif [ "$#" -ne 0 ]; then
    echo "Usage: $0 [--nouveau-userspace]" >&2
    exit 2
fi

GLIBC_CC="${ROOT_DIR}/toolchain/x86_64-linux-glibc/bin/x86_64-linux-gcc"
if [ ! -x "${GLIBC_CC}" ]; then
    echo "[*] Setting up the prebuilt GCC cross-compiler..."
    sh "${ROOT_DIR}/scripts/glibc-toolchain.sh"
fi

check_glibc_244() {
    local glibc_lib="${ROOTFS_DIR}/usr/lib/libc.so.6"
    if [ ! -e "${glibc_lib}" ]; then
        echo "[!] Axon rootfs did not provide ${glibc_lib}." >&2
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
    print(f"[!] Expected Axon's prebuilt glibc 2.44, found {version}.", file=sys.stderr)
    raise SystemExit(1)
PY
}

# Axon's desktop profile checks for avory-login.elf before installing packages.
# Seed only the minimal target first so the login can be linked against the
# actual Axon glibc sysroot, then rerun rootfs with the desktop profile.
"${AXON_BIN}" rootfs --root "${ROOTFS_DIR}" --sync --profile minimal
"${AXON_BIN}" --root "${ROOTFS_DIR}" \
    --mirrorlist "${ROOTFS_DIR}/var/lib/axon/sync/rootfs-mirrors" \
    get --needed --noconfirm --keep-cache libxcrypt
check_glibc_244

"${GLIBC_CC}" -O2 -Wall -Wextra -fno-stack-protector \
    --sysroot="${ROOTFS_DIR}" \
    -Wl,-T,"${ROOT_DIR}/config/userland-link.ld" \
    "${ROOT_DIR}/userland/avory-login.c" -lcrypt \
    -o "${ROOT_DIR}/userland/avory-login.elf"

"${AXON_BIN}" rootfs --root "${ROOTFS_DIR}" \
    --profile desktop --source "${ROOT_DIR}" \
    --packages-from "${ROOT_DIR}/scripts/rootfs-packages.txt"
check_glibc_244

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
