#!/usr/bin/env bash
# Build the minimal AvoryOS root filesystem and disk image with Axon packages:
# Bash/Coreutils, Xorg, IceWM, D-Bus and xterm.
#
# The regular setup-rootfs target builds the full desktop rootfs. This script
# keeps an independent minimal rootfs under build/minimal/.
#
# Usage:
#   scripts/build-minimal.sh rootfs   # build/refresh build/minimal/rootfs
#   scripts/build-minimal.sh disk     # rootfs + build/minimal/disk.img
#   scripts/build-minimal.sh clean    # remove build/minimal
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${ROOT_DIR}/build/minimal"
ROOTFS_DIR="${BUILD_DIR}/rootfs"
PART_IMG="${BUILD_DIR}/part.img"
DISK_IMG="${BUILD_DIR}/disk.img"
STAMP="${ROOTFS_DIR}/.avoryos-minimal"
POPULATE_SCRIPT="${ROOT_DIR}/scripts/populate-ext2-dir.sh"

source "${ROOT_DIR}/scripts/axon-common.sh"

MACHINE_ID="b08040a9e7114fea9634b7f94da7e722"

log() { echo "[*] $*" >&2; }
die() { echo "[!] $*" >&2; exit 1; }

mkdir -p "$BUILD_DIR" "$ROOTFS_DIR"

install_packages() {
    ensure_axon
    log "Provisioning minimal rootfs with Axon..."
    "${AXON_BIN}" rootfs --root "${ROOTFS_DIR}" --sync --profile minimal \
        --packages-from "${ROOT_DIR}/scripts/minimal-rootfs-packages.txt"
}

target_run() {
    unshare -rm chroot "$ROOTFS_DIR" "$@"
}

configure_rootfs() {
    log "Writing AvoryOS Minimal configuration..."

    mkdir -p \
        "${ROOTFS_DIR}/etc/X11/xorg.conf.d" \
        "${ROOTFS_DIR}/proc" \
        "${ROOTFS_DIR}/sys" \
        "${ROOTFS_DIR}/tmp" \
        "${ROOTFS_DIR}/root/.icewm" \
        "${ROOTFS_DIR}/run/dbus" \
        "${ROOTFS_DIR}/run/user/0" \
        "${ROOTFS_DIR}/usr/local/bin" \
        "${ROOTFS_DIR}/var/lib/dbus" \
        "${ROOTFS_DIR}/var/log"

    # D-Bus system service account (the full rootfs adds this in
    # configure-accounts.sh; the minimal image does not use that setup).
    if ! grep -q '^messagebus:' "${ROOTFS_DIR}/etc/group"; then
        echo 'messagebus:x:86:' >> "${ROOTFS_DIR}/etc/group"
    fi
    if ! grep -q '^messagebus:' "${ROOTFS_DIR}/etc/passwd"; then
        echo 'messagebus:!:86:86:D-Bus Message Bus User:/var/run/dbus:/bin/false' \
            >> "${ROOTFS_DIR}/etc/passwd"
    fi

    # Root logs in with bash; the console is a bash session either way.
    awk -F: 'BEGIN { OFS = ":" } $1 == "root" { $7 = "/bin/bash" } { print }' \
        "${ROOTFS_DIR}/etc/passwd" > "${ROOTFS_DIR}/etc/passwd.new"
    mv -f "${ROOTFS_DIR}/etc/passwd.new" "${ROOTFS_DIR}/etc/passwd"

    cat > "${ROOTFS_DIR}/etc/hostname" <<'EOF'
avoryos-minimal
EOF
    cat > "${ROOTFS_DIR}/etc/hosts" <<'EOF'
127.0.0.1 localhost avoryos-minimal
::1 localhost
EOF

    # QEMU user networking (slirp) resolver; harmless elsewhere.
    rm -f "${ROOTFS_DIR}/etc/resolv.conf"
    cat > "${ROOTFS_DIR}/etc/resolv.conf" <<'EOF'
nameserver 10.0.2.3
EOF

    # D-Bus and other services need a stable machine-id.
    rm -f "${ROOTFS_DIR}/etc/machine-id"
    printf '%s\n' "$MACHINE_ID" > "${ROOTFS_DIR}/etc/machine-id"
    ln -sfn /etc/machine-id "${ROOTFS_DIR}/var/lib/dbus/machine-id"

    # BusyBox init uses a small inittab instead of a separate service manager.
    # The AvoryOS kernel only exposes /dev/tty1, so X runs there; a dead
    # session is respawned instead of dropping to a missing console. The
    # rescue boot entry uses init=/bin/bash.
    cat > "${ROOTFS_DIR}/etc/inittab" <<'EOF'
# /etc/inittab for AvoryOS Minimal

::sysinit:/bin/mount -t proc proc /proc
::sysinit:/bin/mount -t sysfs sysfs /sys
::sysinit:/bin/mkdir -p /run/dbus /run/user/0 /tmp/.X11-unix

# Xorg + IceWM on tty1 (the only tty the AvoryOS kernel exposes)
tty1::respawn:/usr/local/bin/startx-icewm

# Shutdown & reboot
::shutdown:/bin/umount -a -r
::ctrlaltdel:/sbin/reboot
EOF
    ln -sf /usr/bin/busybox "${ROOTFS_DIR}/sbin/init"
    ln -sf /usr/bin/busybox "${ROOTFS_DIR}/sbin/reboot"

    # X session launcher: start the system bus, then give IceWM a session bus.
    cat > "${ROOTFS_DIR}/usr/local/bin/startx-icewm" <<'EOF'
#!/bin/sh
# AvoryOS Minimal: start Xorg + IceWM on tty1.  busybox init respawns this
# script whenever the X session exits, so the desktop comes back by itself.
export HOME=/root
export PATH="/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin"
export TERM=linux

export XDG_RUNTIME_DIR=/run/user/0
mkdir -p "$XDG_RUNTIME_DIR"
chmod 0700 "$XDG_RUNTIME_DIR"

# Start the system bus before the desktop session.
if [ ! -S /run/dbus/system_bus_socket ] && command -v dbus-daemon >/dev/null 2>&1; then
    mkdir -p /run/dbus
    dbus-daemon --system --fork 2>/dev/null || true
fi

# A crashed X server can leave its lock behind; a respawn must not trip over it.
rm -f /tmp/.X0-lock /tmp/.X11-unix/X0

exec dbus-run-session -- startx
EOF
    chmod 0755 "${ROOTFS_DIR}/usr/local/bin/startx-icewm"

    cat > "${ROOTFS_DIR}/root/.xinitrc" <<'EOF'
#!/bin/sh
# AvoryOS Minimal X11 session: IceWM.
# Root's home is "/" on AvoryOS: join the XDG defaults through "${HOME%/}"
# so the session never exports //.cache-style paths (WebKitGTK rejects those
# sandbox roots; Epiphany aborts on the resulting critical).
_avory_home="${HOME:-/}"
_avory_home="${_avory_home%/}"
export XDG_CONFIG_HOME="${XDG_CONFIG_HOME:-${_avory_home}/.config}"
export XDG_CACHE_HOME="${XDG_CACHE_HOME:-${_avory_home}/.cache}"
export XDG_DATA_HOME="${XDG_DATA_HOME:-${_avory_home}/.local/share}"
export XDG_STATE_HOME="${XDG_STATE_HOME:-${_avory_home}/.local/state}"
unset _avory_home
if [ -f /etc/X11/xinit/Xresources ]; then
    xrdb -merge /etc/X11/xinit/Xresources 2>/dev/null || true
fi
if command -v dbus-update-activation-environment >/dev/null 2>&1; then
    dbus-update-activation-environment --all 2>/dev/null || true
fi
exec icewm
EOF
    chmod 0755 "${ROOTFS_DIR}/root/.xinitrc"

    cat > "${ROOTFS_DIR}/root/.icewm/menu" <<'EOF'
prog "Terminal (xterm)" utilities-terminal xterm
separator
restart "Restart IceWM" icewm icewm
EOF

    # Xorg on AvoryOS: the kernel's native DRM is card0, input comes from the
    # kernel evdev nodes.  Same configuration the regular rootfs uses.
    cat > "${ROOTFS_DIR}/etc/X11/xorg.conf.d/10-modesetting.conf" <<'EOF'
Section "ServerLayout"
    Identifier  "AvoryLayout"
    Screen      0 "Screen0" 0 0
    InputDevice "Keyboard0" "CoreKeyboard"
    InputDevice "Mouse0" "CorePointer"
    Option      "AutoAddDevices" "false"
EndSection

Section "Device"
    Identifier  "Card0"
    Driver      "modesetting"
    Option      "SWcursor" "true"
EndSection

Section "Screen"
    Identifier  "Screen0"
    Device      "Card0"
EndSection

Section "InputDevice"
    Identifier  "Keyboard0"
    Driver      "evdev"
    Option      "Device" "/dev/input/event0"
    Option      "CoreKeyboard" "true"
EndSection

Section "InputDevice"
    Identifier  "Mouse0"
    Driver      "evdev"
    Option      "Device" "/dev/input/event1"
    Option      "CorePointer" "true"
EndSection
EOF

    # Write AvoryOS identity to Arch's usr/lib/os-release target.
    cat > "${ROOTFS_DIR}/usr/lib/os-release" <<'EOF'
NAME="AvoryOS Minimal"
ID=avoryos
ID_LIKE=arch
VERSION="1.0 (Minimal)"
VERSION_ID=1.0-minimal
PRETTY_NAME="AvoryOS Minimal (bash + Xorg + IceWM)"
HOME_URL="https://github.com/Hidotu-Labs/AvoryOS"
EOF
    rm -f "${ROOTFS_DIR}/etc/os-release"
    ln -sfn ../usr/lib/os-release "${ROOTFS_DIR}/etc/os-release"
}

build_caches() {
    if [ -x "${ROOTFS_DIR}/usr/bin/fc-cache" ]; then
        log "Building fontconfig caches..."
        target_run /usr/bin/fc-cache -f 2>/dev/null || true
    fi
    if [ -x "${ROOTFS_DIR}/usr/bin/gdk-pixbuf-query-loaders" ]; then
        target_run /usr/bin/gdk-pixbuf-query-loaders --update-cache 2>/dev/null || true
    fi
}

build_rootfs() {
    if [ ! -f "$STAMP" ] || [ -f "${ROOTFS_DIR}/etc/alpine-release" ]; then
        rm -rf "$ROOTFS_DIR"
        mkdir -p "$ROOTFS_DIR"
    else
        log "Rootfs already built (${STAMP}); checking packages and refreshing configuration."
    fi
    install_packages
    configure_rootfs
    build_caches
    touch "$STAMP"
    log "Minimal rootfs ready: ${ROOTFS_DIR}"
}

build_disk() {
    build_rootfs

    [ -x "$POPULATE_SCRIPT" ] || die "Missing ${POPULATE_SCRIPT}"

    local root_mb entries part_mb
    root_mb=$(du -sm "$ROOTFS_DIR" | cut -f1)
    entries=$(find "$ROOTFS_DIR" | wc -l)
    part_mb=$(( root_mb + root_mb / 4 + 32 ))
    log "Rootfs is ${root_mb} MiB / ${entries} entries; building a ${part_mb} MiB ext4 partition..."

    # Same on-disk format the kernel is validated against (see GNUmakefile):
    # 1 KiB blocks, no 64bit/metadata_csum/flex_bg features.
    rm -f "$PART_IMG"
    dd if=/dev/zero of="$PART_IMG" bs=1M count="$part_mb" status=none
    mkfs.ext4 -q -F -b 1024 -I 128 \
        -N $(( entries + entries / 4 + 1024 )) \
        -O extent,filetype,has_journal,dir_index,^64bit,^metadata_csum,^flex_bg,^huge_file,^dir_nlink,^extra_isize,^metadata_csum_seed,^orphan_file \
        "$PART_IMG"
    "$POPULATE_SCRIPT" "$PART_IMG" "$ROOTFS_DIR" "/"

    # MBR wrapper with partition 1 at the standard 1 MiB offset; the kernel's
    # ramdisk driver exposes the first partition as root.
    log "Assembling ${DISK_IMG} (MBR + ext4 partition)..."
    rm -f "$DISK_IMG"
    dd if=/dev/zero of="$DISK_IMG" bs=1M count=$(( part_mb + 1 )) status=none
    echo '2048,,L,*' | sfdisk "$DISK_IMG" >/dev/null 2>&1
    dd if="$PART_IMG" of="$DISK_IMG" bs=1M seek=1 conv=notrunc status=none
    rm -f "$PART_IMG"

    log "Minimal disk image ready: ${DISK_IMG}"
}

case "${1:-rootfs}" in
    rootfs)
        build_rootfs
        ;;
    disk)
        build_disk
        ;;
    clean)
        log "Removing ${BUILD_DIR}..."
        rm -rf "$BUILD_DIR"
        ;;
    *)
        echo "Usage: $0 [rootfs|disk|clean]" >&2
        exit 1
        ;;
esac
