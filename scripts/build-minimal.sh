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
    make -C "${AXON_SOURCE_DIR}" axon
    axon_rootfs_sync "${ROOTFS_DIR}"
    axon_rootfs_bootstrap "${ROOTFS_DIR}"
    axon_rootfs_install_list "${ROOTFS_DIR}" \
        "${ROOT_DIR}/scripts/minimal-rootfs-packages.txt"
    bash "${ROOT_DIR}/scripts/configure-pacman.sh" "${ROOTFS_DIR}"
}

install_avory_tools() {
    local glibc_cc="${ROOT_DIR}/toolchain/x86_64-linux-glibc/bin/x86_64-linux-gcc"
    local link_ld="${ROOT_DIR}/config/userland-link.ld"
    if [ ! -x "$glibc_cc" ]; then
        log "Setting up the prebuilt GCC cross-compiler..."
        bash "${ROOT_DIR}/scripts/glibc-toolchain.sh"
    fi

    log "Building target Axon package manager for AvoryOS Minimal..."
    mkdir -p "${ROOTFS_DIR}/usr/bin" "${ROOTFS_DIR}/etc/axon" "${ROOTFS_DIR}/var/cache/axon/pkg"
    rm -f "${ROOTFS_DIR}/usr/bin/axon"
    "$glibc_cc" -std=c11 -O2 -D_GNU_SOURCE -fno-stack-protector \
        --sysroot="${ROOTFS_DIR}" -Wl,-T,"${link_ld}" \
        -I"${AXON_SOURCE_DIR}/include" -I"${AXON_SOURCE_DIR}/src" \
        "${AXON_SOURCE_DIR}/src/main.c" "${AXON_SOURCE_DIR}/src/config.c" \
        "${AXON_SOURCE_DIR}"/src/cli/*.c "${AXON_SOURCE_DIR}"/src/package/*.c \
        "${AXON_SOURCE_DIR}"/src/repository/*.c "${AXON_SOURCE_DIR}"/src/database/*.c \
        "${AXON_SOURCE_DIR}"/src/resolver/*.c "${AXON_SOURCE_DIR}"/src/transaction/*.c \
        "${AXON_SOURCE_DIR}"/src/security/*.c "${AXON_SOURCE_DIR}"/src/runtime/*.c \
        -larchive -lcurl -o "${ROOTFS_DIR}/usr/bin/axon"
    chmod 0755 "${ROOTFS_DIR}/usr/bin/axon"
    if [ -f "${AXON_SOURCE_DIR}/axon.conf" ]; then
        install -m 0644 "${AXON_SOURCE_DIR}/axon.conf" "${ROOTFS_DIR}/etc/axon/axon.conf"
    fi

    log "Building AvoryOS TTY installer for AvoryOS Minimal..."
    rm -f "${ROOTFS_DIR}/usr/bin/avory-installer" "${ROOTFS_DIR}/usr/bin/install-avoryos"
    "$glibc_cc" -O2 -Wall -Wextra -fno-stack-protector \
        --sysroot="${ROOTFS_DIR}" -Wl,-T,"${link_ld}" \
        -I"${ROOT_DIR}/userland/installer" \
        "${ROOT_DIR}/userland/installer/main.c" \
        "${ROOT_DIR}/userland/installer/tui.c" \
        "${ROOT_DIR}/userland/installer/disks.c" \
        "${ROOT_DIR}/userland/installer/install.c" \
        "${ROOT_DIR}/userland/installer/packages.c" \
        -o "${ROOTFS_DIR}/usr/bin/avory-installer"
    chmod 0755 "${ROOTFS_DIR}/usr/bin/avory-installer"
    ln -sfn avory-installer "${ROOTFS_DIR}/usr/bin/install-avoryos"

    log "Building DRM device discovery tool for AvoryOS Minimal..."
    rm -f "${ROOTFS_DIR}/usr/bin/drm-identify"
    "$glibc_cc" -O2 -Wall -Wextra -fno-stack-protector \
        --sysroot="${ROOTFS_DIR}" -Wl,-T,"${link_ld}" \
        -I"${ROOT_DIR}/kernel/linux/include/uapi/drm" \
        "${ROOT_DIR}/userland/drm_identify.c" \
        -o "${ROOTFS_DIR}/usr/bin/drm-identify"
    chmod 0755 "${ROOTFS_DIR}/usr/bin/drm-identify"
    install -m 0755 "${ROOT_DIR}/initrd/drm-pick.sh" "${ROOTFS_DIR}/usr/bin/drm-pick.sh"

    if [ -f "${ROOT_DIR}/assets/boo.png" ] && [ -d "${ROOTFS_DIR}/usr/share/limine" ]; then
        install -m 0644 "${ROOT_DIR}/assets/boo.png" "${ROOTFS_DIR}/usr/share/limine/boo.png"
    fi
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

    # QEMU user networking (slirp) resolver; overwritten by udhcpc on real LANs.
    rm -f "${ROOTFS_DIR}/etc/resolv.conf"
    cat > "${ROOTFS_DIR}/etc/resolv.conf" <<'EOF'
nameserver 10.0.2.3
EOF

    # BusyBox networking applets (ping, ifconfig, route, udhcpc, etc.) and
    # the udhcpc lease script that applies IP, default route, and resolv.conf.
    install -D -m 0755 "${ROOT_DIR}/config/init/udhcpc.script" \
        "${ROOTFS_DIR}/usr/share/udhcpc/default.script"
    for cmd in ping ping6 ifconfig route ip udhcpc nslookup traceroute; do
        if [ ! -e "${ROOTFS_DIR}/usr/bin/${cmd}" ] && [ ! -e "${ROOTFS_DIR}/usr/sbin/${cmd}" ]; then
            ln -sfn /usr/bin/busybox "${ROOTFS_DIR}/usr/bin/${cmd}"
        fi
    done

    # Stage Realtek NIC and AMDGPU firmware blobs when available locally so
    # hardware drivers can initialize without falling back to software.
    if [ -d "${ROOT_DIR}/build/linux-firmware/rtl_nic" ]; then
        mkdir -p "${ROOTFS_DIR}/lib/firmware/rtl_nic"
        for fw in "${ROOT_DIR}"/build/linux-firmware/rtl_nic/rtl810*.fw \
                  "${ROOT_DIR}"/build/linux-firmware/rtl_nic/rtl8168*.fw; do
            [ -f "$fw" ] && install -m 0644 "$fw" "${ROOTFS_DIR}/lib/firmware/rtl_nic/"
        done
    fi
    if [ -d "${ROOT_DIR}/build/linux-firmware/amdgpu" ]; then
        mkdir -p "${ROOTFS_DIR}/lib/firmware/amdgpu"
        cp -f "${ROOT_DIR}"/build/linux-firmware/amdgpu/*.bin "${ROOTFS_DIR}/lib/firmware/amdgpu/"
        chmod 0644 "${ROOTFS_DIR}"/lib/firmware/amdgpu/*.bin
    elif [ -d "${ROOT_DIR}/build/firmware/lib/firmware/amdgpu" ]; then
        mkdir -p "${ROOTFS_DIR}/lib/firmware/amdgpu"
        cp -f "${ROOT_DIR}"/build/firmware/lib/firmware/amdgpu/*.bin "${ROOTFS_DIR}/lib/firmware/amdgpu/"
        chmod 0644 "${ROOTFS_DIR}"/lib/firmware/amdgpu/*.bin
    fi

    cat > "${ROOTFS_DIR}/usr/local/bin/avory-net-init" <<'EOF'
#!/bin/sh
# Bring up loopback and start BusyBox udhcpc on wired interfaces unless disabled.
BB=/usr/bin/busybox
[ -x "$BB" ] || BB=busybox

"$BB" ifconfig lo 127.0.0.1 up 2>/dev/null || true
[ -f /etc/avory-no-dhcp ] && exit 0

ifaces=""
for dev in /sys/class/net/eth* /sys/class/net/en*; do
    [ -e "$dev" ] && ifaces="$ifaces $(basename "$dev")"
done
if [ -z "$ifaces" ]; then
    ifaces="eth0"
fi

for iface in $ifaces; do
    "$BB" ifconfig "$iface" up 2>/dev/null || continue
    "$BB" udhcpc -b -t 3 -T 1 -A 2 -i "$iface" \
        -s /usr/share/udhcpc/default.script \
        -p "/run/udhcpc.${iface}.pid" >/var/log/udhcpc.log 2>&1 || true
done
EOF
    chmod 0755 "${ROOTFS_DIR}/usr/local/bin/avory-net-init"

    cat > "${ROOTFS_DIR}/usr/local/bin/avory-xorg-init" <<'EOF'
#!/bin/sh
# Select the active DRM display card for Xorg (preferring amdgpu with glamor
# over the kernel's software ascentdrm card0 fallback).
CONF="/etc/X11/xorg.conf.d/10-modesetting.conf"
IDENTIFY="/bin/drm-identify"
PICK="/bin/drm-pick.sh"

[ -x "$IDENTIFY" ] || exit 0
inv="$("$IDENTIFY" 2>/dev/null || true)"
[ -n "$inv" ] || exit 0

card=""
if [ -x "$PICK" ]; then
    card="$("$PICK" --display 2>/dev/null || true)"
fi
if [ -z "$card" ]; then
    card="$(printf '%s\n' "$inv" | awk '$2=="amdgpu" && $4==1 {print $1; exit}')"
fi
if [ -z "$card" ]; then
    card="$(printf '%s\n' "$inv" | awk '$4==1 {print $1; exit}')"
fi
[ -n "$card" ] || card="/dev/dri/card0"

driver="$(printf '%s\n' "$inv" | awk -v c="$card" '$1==c {print $2; exit}')"
render="$(printf '%s\n' "$inv" | awk -v c="$card" '$1==c {print $5; exit}')"

accel="none"
swcursor="true"
pageflip=""
if [ "$driver" = "amdgpu" ] && [ -n "$render" ] && [ "$render" != "-" ] &&
   [ ! -f /etc/avory-no-glamor ] && ! grep -qw 'avory_no_glamor=1' /proc/cmdline 2>/dev/null; then
    accel="glamor"
    swcursor="false"
    pageflip='    Option      "PageFlip" "false"'
elif [ "$driver" = "virtio_gpu" ] && [ -n "$render" ] && [ "$render" != "-" ]; then
    accel="glamor"
fi

mkdir -p /etc/X11/xorg.conf.d
cat > "${CONF}.tmp" <<XEOF
Section "OutputClass"
    Identifier  "AMDPrimary"
    MatchDriver "amdgpu"
    Driver      "modesetting"
    Option      "PrimaryGPU" "true"
EndSection

Section "ServerLayout"
    Identifier  "AvoryLayout"
    Screen      0 "Screen0" 0 0
    InputDevice "Keyboard0" "CoreKeyboard"
    InputDevice "Mouse0" "CorePointer"
    Option      "AutoAddDevices" "false"
    Option      "AutoAddGPU" "false"
    Option      "AutoBindGPU" "false"
EndSection

Section "Device"
    Identifier  "Card0"
    Driver      "modesetting"
    Option      "kmsdev" "${card}"
    Option      "AccelMethod" "${accel}"
    Option      "SWcursor" "${swcursor}"
${pageflip}
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
XEOF
mv -f "${CONF}.tmp" "$CONF"
EOF
    chmod 0755 "${ROOTFS_DIR}/usr/local/bin/avory-xorg-init"

    cat > "${ROOTFS_DIR}/usr/local/bin/gpu-diag" <<'EOF'
#!/bin/sh
{
echo "=== DRM devices ==="
ls -la /dev/dri 2>&1 || true
for device in /sys/class/drm/card*/device; do
    [ -r "$device/vendor" ] && [ -r "$device/device" ] || continue
    vendor=$(cat "$device/vendor")
    id=$(cat "$device/device")
    driver=$(readlink "$device/driver" 2>/dev/null || true)
    printf '%s vendor=%s device=%s driver=%s\n' \
        "${device%/device}" "$vendor" "$id" "${driver##*/}"
done
echo "=== klog ==="
if [ -r /proc/klog ]; then
    tail -c 65536 /proc/klog 2>/dev/null | grep -iE "VFCT|ACPI|VBIOS|amdgpu|drm|firmware|failed|SIGSEGV|ldconfig" | tail -n 35
fi
echo "=== Xorg.0.log ==="
if [ -r /var/log/Xorg.0.log ]; then
    grep -E "\(EE\)|\(WW\)|glamor|amdgpu|DRI" /var/log/Xorg.0.log 2>/dev/null | tail -n 30
fi
if [ -r /var/log/Xorg.0.log.glamor ]; then
    echo "=== Xorg.0.log.glamor (EE) ==="
    grep -E "\(EE\)|glamor|amdgpu|DRI" /var/log/Xorg.0.log.glamor 2>/dev/null | tail -n 20
fi
echo "=== cpu / ps ==="
head -n 5 /proc/stat 2>/dev/null || true
if command -v ps >/dev/null 2>&1; then
    ps -eo pid,ppid,stat,pcpu,comm --sort=-pcpu 2>/dev/null | head -n 15 || ps aux 2>/dev/null | head -n 15 || ps 2>/dev/null | head -n 15
fi
echo "=== glxinfo -B ==="
if [ "${1:-}" = "--gl" ]; then
    if command -v glxinfo >/dev/null 2>&1 && [ -n "${DISPLAY:-}" ]; then
        if command -v timeout >/dev/null 2>&1; then
            LIBGL_DEBUG=verbose timeout 3 glxinfo -B 2>&1 | head -n 30
        else
            LIBGL_DEBUG=verbose glxinfo -B 2>&1 | head -n 30
        fi
    else
        echo "(glxinfo unavailable or DISPLAY unset)"
    fi
else
    echo "(skipped to avoid touching amdgpu GLX; run 'gpu-diag --gl' to test)"
fi
} 2>&1 | tee /var/log/gpu-diag.log
EOF
    chmod 0755 "${ROOTFS_DIR}/usr/local/bin/gpu-diag"
    install -m 0755 "${ROOT_DIR}/scripts/tests/capture-glx-stall.sh" \
        "${ROOTFS_DIR}/usr/local/bin/glxstall"

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
::sysinit:/bin/mkdir -p /dev/pts /dev/shm
::sysinit:/bin/mount -t devpts devpts /dev/pts
::sysinit:/bin/mount -t tmpfs -o rw,nosuid,nodev,mode=1777 tmpfs /dev/shm
::sysinit:/bin/mount -t tmpfs -o rw,nosuid,nodev,mode=1777 tmpfs /tmp
::sysinit:/bin/mount -t tmpfs -o rw,nosuid,nodev,mode=0755 tmpfs /run
::sysinit:/bin/mkdir -p /run/dbus /run/user/0 /tmp/.X11-unix
::sysinit:/usr/local/bin/avory-net-init
::sysinit:/usr/local/bin/avory-xorg-init

# Xorg + IceWM on tty1 (the only tty the AvoryOS kernel exposes)
tty1::respawn:/usr/local/bin/startx-icewm

# Shutdown & reboot
::shutdown:/bin/umount -a -r
::ctrlaltdel:/sbin/reboot
EOF
    ln -sf /usr/bin/busybox "${ROOTFS_DIR}/sbin/init"
    ln -sf /usr/bin/busybox "${ROOTFS_DIR}/sbin/reboot"
    ln -sf /usr/bin/busybox "${ROOTFS_DIR}/sbin/poweroff"
    ln -sf /usr/bin/busybox "${ROOTFS_DIR}/sbin/halt"

    cat > "${ROOTFS_DIR}/sbin/shutdown" <<'EOF'
#!/bin/sh
mode="poweroff"
for arg in "$@"; do
    case "$arg" in
        -r|--reboot) mode="reboot" ;;
        -h|-P|--poweroff) mode="poweroff" ;;
        -H|--halt) mode="halt" ;;
    esac
done
exec "/sbin/${mode}"
EOF
    chmod 0755 "${ROOTFS_DIR}/sbin/shutdown"

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

if grep -qw avory_installer /proc/cmdline 2>/dev/null; then
    /usr/sbin/avory-installer || /bin/avory-installer
    exec /bin/bash -l
fi

# Start the system bus before the desktop session.
if [ ! -S /run/dbus/system_bus_socket ] && command -v dbus-daemon >/dev/null 2>&1; then
    mkdir -p /run/dbus
    dbus-daemon --system --fork 2>/dev/null || true
fi

# A crashed X server can leave its lock behind; a respawn must not trip over it.
rm -f /tmp/.X0-lock /tmp/.X11-unix/X0

[ -x /usr/local/bin/avory-xorg-init ] && /usr/local/bin/avory-xorg-init || true

CONF=/etc/X11/xorg.conf.d/10-modesetting.conf
dbus-run-session -- startx
rc=$?
if [ "$rc" -ne 0 ] && [ -f "$CONF" ] && grep -q '"glamor"' "$CONF"; then
    [ -f /var/log/Xorg.0.log ] && cp -f /var/log/Xorg.0.log /var/log/Xorg.0.log.glamor
    sed -e 's/"glamor"/"none"/g' \
        -e 's/"SWcursor"[[:space:]]*"false"/"SWcursor"    "true"/g' \
        -e '/"PageFlip"/d' \
        "$CONF" > "${CONF}.tmp" && mv -f "${CONF}.tmp" "$CONF"
    rm -f /tmp/.X0-lock /tmp/.X11-unix/X0
    dbus-run-session -- startx
    rc=$?
fi
if [ "$rc" -ne 0 ]; then
    echo "=== Xorg failed (rc=$rc); stopping respawn ==="
    command -v drm-identify >/dev/null 2>&1 && drm-identify 2>&1 || true
    [ -r /var/log/Xorg.0.log ] && grep -E '\(EE\)|\(WW\)|modeset|amdgpu|glamor|Fatal' /var/log/Xorg.0.log | tail -n 25
    echo "=== Dropping to rescue shell on tty1 (run startx-icewm to retry) ==="
    exec /bin/bash -l
fi
exit "$rc"
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

    if [ -f "${ROOTFS_DIR}/etc/X11/xinit/xserverrc" ]; then
        chmod 0755 "${ROOTFS_DIR}/etc/X11/xinit/xserverrc"
    fi

    cat > "${ROOTFS_DIR}/usr/local/bin/icewm-dynamic-menu" <<'EOF'
#!/bin/sh
# Dynamically generate IceWM menu entries from installed .desktop files.
set --
for f in /usr/share/applications/*.desktop /usr/local/share/applications/*.desktop; do
    [ -f "$f" ] && set -- "$@" "$f"
done
[ $# -eq 0 ] && exit 0
exec awk '
function trim(s) {
    gsub(/^[ \t\r\n]+|[ \t\r\n]+$/, "", s)
    return s
}
function escape_q(s) {
    gsub(/"/, "\\\"", s)
    return s
}
function map_cat(cats,    c) {
    c = ";" cats ";"
    if (c ~ /;Network;|;WebBrowser;|;Email;/) return "Internet"
    if (c ~ /;AudioVideo;|;Audio;|;Video;|;Player;/) return "Multimedia"
    if (c ~ /;Graphics;|;2DGraphics;|;3DGraphics;/) return "Graphics"
    if (c ~ /;Development;|;IDE;/) return "Development"
    if (c ~ /;Office;|;TextEditor;/) return "Office"
    if (c ~ /;Game;/) return "Games"
    if (c ~ /;System;|;Settings;|;Monitor;/) return "System"
    if (c ~ /;Utility;|;Accessory;|;TerminalEmulator;|;FileManager;/) return "Utilities"
    return "Other"
}
function flush_entry() {
    if (in_desktop && !nodisplay && type == "Application" && name != "" && exec_cmd != "") {
        gsub(/%[uUfFick]/, "", exec_cmd)
        exec_cmd = trim(exec_cmd)
        if (exec_cmd != "") {
            if (term == "true") exec_cmd = "xterm -e " exec_cmd
            if (icon == "") icon = "application-x-executable"
            cat = map_cat(cats)
            line = "  prog \"" escape_q(name) "\" " icon " " exec_cmd
            items[cat] = items[cat] line "\n"
        }
    }
    in_desktop = 0; nodisplay = 0; type = ""; name = ""; icon = ""; exec_cmd = ""; cats = ""; term = ""
}
BEGIN {
    in_desktop = 0
}
FNR == 1 {
    flush_entry()
}
/^\[Desktop Entry\]/ {
    flush_entry()
    in_desktop = 1
    next
}
/^\[/ {
    if (in_desktop) flush_entry()
    next
}
in_desktop {
    pos = index($0, "=")
    if (pos == 0) next
    key = trim(substr($0, 1, pos - 1))
    val = trim(substr($0, pos + 1))
    if (key == "Type") type = val
    else if (key == "Name" && name == "") name = val
    else if (key == "Icon" && icon == "") icon = val
    else if (key == "Exec" && exec_cmd == "") exec_cmd = val
    else if (key == "Categories") cats = val
    else if (key == "Terminal") term = tolower(val)
    else if ((key == "NoDisplay" || key == "Hidden") && tolower(val) == "true") nodisplay = 1
}
END {
    flush_entry()
    order[1] = "Internet";    icons["Internet"] = "applications-internet"
    order[2] = "Utilities";   icons["Utilities"] = "applications-accessories"
    order[3] = "System";      icons["System"] = "applications-system"
    order[4] = "Office";      icons["Office"] = "applications-office"
    order[5] = "Graphics";    icons["Graphics"] = "applications-graphics"
    order[6] = "Multimedia";  icons["Multimedia"] = "applications-multimedia"
    order[7] = "Development"; icons["Development"] = "applications-development"
    order[8] = "Games";       icons["Games"] = "applications-games"
    order[9] = "Other";       icons["Other"] = "applications-other"
    for (i = 1; i <= 9; i++) {
        c = order[i]
        if (items[c] != "") {
            printf "menu \"%s\" %s {\n%s}\n", c, icons[c], items[c]
        }
    }
}
' "$@" 2>/dev/null
EOF
    chmod 0755 "${ROOTFS_DIR}/usr/local/bin/icewm-dynamic-menu"

    cat > "${ROOTFS_DIR}/root/.icewm/menu" <<'EOF'
menuprog "Applications" folder /usr/local/bin/icewm-dynamic-menu
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
    if [ "${MINIMAL_SKIP_PACKAGES:-0}" != "1" ]; then
        install_packages
    fi
    install_avory_tools
    configure_rootfs
    build_caches
    touch "$STAMP"
    log "Minimal rootfs ready: ${ROOTFS_DIR}"
}

build_disk() {
    if [ "${MINIMAL_SKIP_ROOTFS:-0}" != "1" ]; then
        build_rootfs
    fi

    [ -x "$POPULATE_SCRIPT" ] || die "Missing ${POPULATE_SCRIPT}"

    # Exclude docs, headers, package archives, non-English locales, and
    # host/build-only binutils/gettext tools from the packed image while
    # keeping ROOTFS_DIR intact for incremental rebuilds.
    local exclude_paths=(
        "usr/include"
        "usr/share/doc"
        "usr/share/man"
        "usr/share/info"
        "usr/share/gtk-doc"
        "usr/share/gir-1.0"
        "usr/share/i18n"
        "usr/lib/gconv/*"
        "!usr/lib/gconv/gconv-modules*"
        "!usr/lib/gconv/gconv-modules.d/*"
        "!usr/lib/gconv/IBM850.so"
        "!usr/lib/gconv/IBM437.so"
        "!usr/lib/gconv/ISO8859-1.so"
        "!usr/lib/gconv/UTF-16.so"
        "!usr/lib/gconv/CP1252.so"
        "!usr/lib/gconv/UNICODE.so"
        "usr/share/locale/*"
        "!usr/share/locale/en*"
        "!usr/share/locale/locale.alias"
        "var/cache/axon/*"
        "!var/cache/axon/pkg"
        "var/cache/axon/pkg/*"
        "var/cache/pacman/*"
        "!var/cache/pacman/pkg"
        "var/cache/pacman/pkg/*"
        "var/lib/pacman/sync/*"
        "var/lib/axon/sync/*"
        "usr/lib/gprofng"
        "usr/lib/libgprofng*"
        "usr/bin/addr2line"
        "usr/bin/ar"
        "usr/bin/as"
        "usr/bin/c++filt"
        "usr/bin/dwp"
        "usr/bin/elfedit"
        "usr/bin/gp-*"
        "usr/bin/gprof"
        "usr/bin/gprofng*"
        "usr/bin/ld"
        "usr/bin/ld.bfd"
        "usr/bin/ld.gold"
        "usr/bin/nm"
        "usr/bin/objcopy"
        "usr/bin/objdump"
        "usr/bin/ranlib"
        "usr/bin/readelf"
        "usr/bin/size"
        "usr/bin/strings"
        "usr/bin/strip"
        "usr/bin/xgettext"
        "usr/bin/msg*"
    )

    local root_mb entries part_mb
    root_mb=$(du -sm \
        --exclude='usr/include' \
        --exclude='usr/share/doc' \
        --exclude='usr/share/man' \
        --exclude='usr/share/info' \
        --exclude='usr/share/gtk-doc' \
        --exclude='usr/share/gir-1.0' \
        --exclude='usr/share/i18n' \
        --exclude='usr/lib/gconv' \
        --exclude='var/cache/axon/pkg/*' \
        --exclude='var/cache/pacman/pkg/*' \
        --exclude='var/lib/pacman/sync/*' \
        --exclude='var/lib/axon/sync/*' \
        --exclude='*.a' \
        "$ROOTFS_DIR" | cut -f1)
    entries=$(find "$ROOTFS_DIR" | wc -l)
    part_mb=$(( root_mb + root_mb / 4 + 32 ))
    log "Packed rootfs is ~${root_mb} MiB (${entries} total entries); building a ${part_mb} MiB ext4 partition..."

    # Same on-disk format the kernel is validated against (see GNUmakefile):
    # 1 KiB blocks, no 64bit/large_file/metadata_csum/flex_bg features.
    rm -f "$PART_IMG"
    dd if=/dev/zero of="$PART_IMG" bs=1M count="$part_mb" status=none
    mkfs.ext4 -q -F -b 1024 -I 128 -m 0 \
        -N $(( entries + entries / 4 + 1024 )) \
        -O extent,filetype,has_journal,dir_index,^64bit,^metadata_csum,^flex_bg,^huge_file,^large_file,^dir_nlink,^extra_isize,^metadata_csum_seed,^orphan_file \
        "$PART_IMG"
    EXCLUDE_IMAGE_PATHS="${exclude_paths[*]}" "$POPULATE_SCRIPT" "$PART_IMG" "$ROOTFS_DIR" "/"

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
