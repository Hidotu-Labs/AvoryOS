#!/usr/bin/env bash
# scripts/build-minimal.sh - Build the minimal AvoryOS root filesystem and
# disk image: GNU bash + GNU coreutils, Xorg, IceWM, OpenRC with D-Bus and st.
#
# The regular build (scripts/setup-alpine.sh + the GNUmakefile disk.img target)
# layers a full desktop rootfs (KDE/XFCE/GTK/VLC/...).  This script produces a
# small, independent rootfs under build/minimal/ that contains only the
# requested stack:
#
#   * OpenRC (busybox init -> /etc/inittab) with the system D-Bus running
#   * GNU bash and GNU coreutils from Alpine
#   * Xorg (modesetting + evdev) auto-starting IceWM on tty1
#   * st terminal
#
# Packages are installed with Alpine's static apk, so dependencies are
# resolved properly.  Package scripts are skipped (--no-scripts): the image is
# assembled unprivileged with debugfs, so caches that matter here (fontconfig,
# gdk-pixbuf) are generated explicitly below.
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
CACHE_DIR="${BUILD_DIR}/cache"
STAMP="${ROOTFS_DIR}/.avoryos-minimal"
POPULATE_SCRIPT="${ROOT_DIR}/scripts/populate-ext2-dir.sh"

# Keep the same Alpine branch as the regular rootfs: the AvoryOS kernel and
# its Linux compat layer are validated against it.
ALPINE_BRANCH="v3.21"
ALPINE_VERSION="3.21.0"
ALPINE_TARBALL="alpine-minirootfs-${ALPINE_VERSION}-x86_64.tar.gz"
ALPINE_URL="https://dl-cdn.alpinelinux.org/alpine/${ALPINE_BRANCH}/releases/x86_64/${ALPINE_TARBALL}"
ALPINE_PKGINDEX_URL="https://dl-cdn.alpinelinux.org/alpine/${ALPINE_BRANCH}/main/x86_64/"

REPOS=(
    "https://dl-cdn.alpinelinux.org/alpine/${ALPINE_BRANCH}/main"
    "https://dl-cdn.alpinelinux.org/alpine/${ALPINE_BRANCH}/community"
)

# Top-level packages; apk pulls the rest of the dependency closure.
PACKAGES=(
    # Base system and init
    alpine-base
    bash
    coreutils
    # OpenRC service scripts for the system bus
    dbus
    dbus-openrc
    dbus-x11
    # Xorg: server, input drivers, session launcher, keyboard data, fonts
    xorg-server
    xf86-input-evdev
    xf86-input-libinput
    xinit
    xauth
    mcookie
    xmodmap
    xrdb
    xkeyboard-config
    xkbcomp
    font-misc-misc
    font-cursor-misc
    font-dejavu
    # Window manager and terminal
    icewm
    st
    # DRM + software GL (glamor needs a DRI driver on the modesetting driver)
    libdrm
    mesa-dri-gallium
)

MACHINE_ID="b08040a9e7114fea9634b7f94da7e722"

log() { echo "[*] $*" >&2; }
die() { echo "[!] $*" >&2; exit 1; }

download() {
    # download URL DEST
    if [ ! -f "$2" ]; then
        log "Downloading $(basename "$2")..."
        curl -fL --retry 3 "$1" -o "$2"
    fi
}

prepare_apk_static() {
    local apk_static="${CACHE_DIR}/apk.static"
    [ -x "$apk_static" ] && { echo "$apk_static"; return 0; }

    mkdir -p "$CACHE_DIR"
    local pkg
    pkg=$(curl -sL "$ALPINE_PKGINDEX_URL" |
        grep -oP 'apk-tools-static-[0-9][^"<>]*\.apk' | sort -V | tail -1)
    [ -n "$pkg" ] || die "Could not find apk-tools-static in ${ALPINE_PKGINDEX_URL}"
    download "${ALPINE_PKGINDEX_URL}${pkg}" "${CACHE_DIR}/${pkg}"

    # The apk tarball carries a checksum xheader tar may warn about; the
    # extraction itself is what matters here.
    tar -xzf "${CACHE_DIR}/${pkg}" -C "$CACHE_DIR" sbin/apk.static 2>/dev/null
    mv -f "${CACHE_DIR}/sbin/apk.static" "$apk_static"
    rm -rf "${CACHE_DIR}/sbin"
    echo "$apk_static"
}

install_packages() {
    local apk_static
    apk_static=$(prepare_apk_static)
    local apk_common=(
        --root "$ROOTFS_DIR"
        --arch x86_64
        --initdb
        --no-scripts
        --allow-untrusted
        --cache-dir "$CACHE_DIR"
    )
    local apk_repo_args=()
    local repo
    for repo in "${REPOS[@]}"; do
        apk_repo_args+=(-X "$repo")
    done

    log "Fetching Alpine package indexes..."
    "$apk_static" "${apk_common[@]}" "${apk_repo_args[@]}" update

    log "Installing: ${PACKAGES[*]}"
    "$apk_static" "${apk_common[@]}" "${apk_repo_args[@]}" add "${PACKAGES[@]}"
}

extract_minirootfs() {
    mkdir -p "$CACHE_DIR"
    # Reuse the tarball the regular build already downloaded when present.
    if [ -f "${ROOT_DIR}/build/alpine/${ALPINE_TARBALL}" ]; then
        cp -f "${ROOT_DIR}/build/alpine/${ALPINE_TARBALL}" "${CACHE_DIR}/${ALPINE_TARBALL}"
    fi
    download "$ALPINE_URL" "${CACHE_DIR}/${ALPINE_TARBALL}"

    log "Extracting Alpine ${ALPINE_VERSION} minirootfs..."
    rm -rf "$ROOTFS_DIR"
    mkdir -p "$ROOTFS_DIR"
    tar -xzf "${CACHE_DIR}/${ALPINE_TARBALL}" -C "$ROOTFS_DIR" 2>/dev/null
}

configure_rootfs() {
    log "Writing AvoryOS Minimal configuration..."

    mkdir -p \
        "${ROOTFS_DIR}/etc/X11/xorg.conf.d" \
        "${ROOTFS_DIR}/etc/apk" \
        "${ROOTFS_DIR}/etc/init.d" \
        "${ROOTFS_DIR}/etc/runlevels/sysinit" \
        "${ROOTFS_DIR}/etc/runlevels/boot" \
        "${ROOTFS_DIR}/etc/runlevels/default" \
        "${ROOTFS_DIR}/etc/runlevels/shutdown" \
        "${ROOTFS_DIR}/root/.icewm" \
        "${ROOTFS_DIR}/run/dbus" \
        "${ROOTFS_DIR}/run/openrc" \
        "${ROOTFS_DIR}/run/user/0" \
        "${ROOTFS_DIR}/usr/local/bin" \
        "${ROOTFS_DIR}/var/cache/apk" \
        "${ROOTFS_DIR}/var/lib/dbus" \
        "${ROOTFS_DIR}/var/log"

    # D-Bus system service account (the full rootfs adds this in
    # configure-accounts.sh; the Alpine minirootfs does not ship it).
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
    cat > "${ROOTFS_DIR}/etc/resolv.conf" <<'EOF'
nameserver 10.0.2.3
EOF

    cat > "${ROOTFS_DIR}/etc/apk/repositories" <<EOF
${REPOS[0]}
${REPOS[1]}
EOF

    # D-Bus and other services need a stable machine-id.
    printf '%s\n' "$MACHINE_ID" > "${ROOTFS_DIR}/etc/machine-id"
    ln -sfn /etc/machine-id "${ROOTFS_DIR}/var/lib/dbus/machine-id"

    # OpenRC: AvoryOS has no cgroups, runs on the kernel-managed /dev and needs
    # rc_sys empty (no container/VM detection).
    cat > "${ROOTFS_DIR}/etc/rc.conf" <<'EOF'
# /etc/rc.conf - OpenRC configuration for AvoryOS Minimal
rc_parallel="NO"
rc_controller_cgroups="NO"
rc_sys=""
rc_tty_number=1
rc_logger="YES"
rc_log_path="/var/log/rc.log"
EOF

    # AvoryOS kernel owns /dev, so provide a no-op `dev` service instead of
    # busybox-mdev's (same approach as the regular rootfs).
    cat > "${ROOTFS_DIR}/etc/init.d/dev" <<'EOF'
#!/sbin/openrc-run
description="AvoryOS kernel-managed /dev"

depend() {
    provide dev
    keyword -shutdown
}

start() {
    return 0
}
EOF
    chmod 0755 "${ROOTFS_DIR}/etc/init.d/dev"

    # Runlevels, mirroring the services the regular AvoryOS rootfs enables.
    local s
    for s in devfs dev dmesg sysfs; do
        [ -f "${ROOTFS_DIR}/etc/init.d/${s}" ] &&
            ln -sfn "/etc/init.d/${s}" "${ROOTFS_DIR}/etc/runlevels/sysinit/${s}"
    done
    for s in bootmisc hostname localmount; do
        [ -f "${ROOTFS_DIR}/etc/init.d/${s}" ] &&
            ln -sfn "/etc/init.d/${s}" "${ROOTFS_DIR}/etc/runlevels/boot/${s}"
    done
    for s in dbus local; do
        [ -f "${ROOTFS_DIR}/etc/init.d/${s}" ] &&
            ln -sfn "/etc/init.d/${s}" "${ROOTFS_DIR}/etc/runlevels/default/${s}"
    done
    for s in killprocs mount-ro; do
        [ -f "${ROOTFS_DIR}/etc/init.d/${s}" ] &&
            ln -sfn "/etc/init.d/${s}" "${ROOTFS_DIR}/etc/runlevels/shutdown/${s}"
    done

    # busybox init.  The AvoryOS kernel only exposes /dev/tty1, so X runs
    # there; a dead session is respawned (X restarts) instead of dropping to a
    # console that does not exist.  The rescue boot entry uses init=/bin/bash.
    cat > "${ROOTFS_DIR}/etc/inittab" <<'EOF'
# /etc/inittab for AvoryOS Minimal

::sysinit:/sbin/openrc sysinit
::sysinit:/sbin/openrc boot
::wait:/sbin/openrc default

# Xorg + IceWM on tty1 (the only tty the AvoryOS kernel exposes)
tty1::respawn:/usr/local/bin/startx-icewm

# Shutdown & reboot
::shutdown:/sbin/openrc shutdown
::ctrlaltdel:/sbin/reboot
EOF

    # X session launcher: wait for the system bus OpenRC started, then give
    # IceWM a session bus of its own.
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

# The OpenRC `dbus` service has already run by the time inittab gets here;
# start the system bus here too if that service is not enabled.
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
prog "Terminal (st)" utilities-terminal st
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

    # /etc/os-release is a symlink to /usr/lib/os-release on Alpine; write the
    # target and keep the symlink layout intact.
    cat > "${ROOTFS_DIR}/usr/lib/os-release" <<'EOF'
NAME="AvoryOS Minimal"
ID=avoryos
ID_LIKE=alpine
VERSION="1.0 (Minimal)"
VERSION_ID=1.0-minimal
PRETTY_NAME="AvoryOS Minimal (bash + Xorg + IceWM)"
HOME_URL="https://github.com/Hidotu-Labs/AvoryOS"
EOF
    rm -f "${ROOTFS_DIR}/etc/os-release"
    ln -sfn ../usr/lib/os-release "${ROOTFS_DIR}/etc/os-release"
}

build_caches() {
    # Generate target-side caches the way the regular rootfs does: run the
    # musl-linked tools through the target loader so no chroot is needed.
    local loader="${ROOTFS_DIR}/lib/ld-musl-x86_64.so.1"
    local fc_cache="${ROOTFS_DIR}/usr/bin/fc-cache"
    if [ -x "$loader" ] && [ -x "$fc_cache" ]; then
        log "Building final fontconfig caches for the target rootfs..."
        mkdir -p "${ROOTFS_DIR}/var/cache/fontconfig"
        "$loader" --library-path "${ROOTFS_DIR}/lib:${ROOTFS_DIR}/usr/lib" \
            "$fc_cache" --sysroot="$ROOTFS_DIR" --really-force --system-only || true
    fi

    # gdk-pixbuf has no way to run gdk-pixbuf-query-loaders against the image
    # unprivileged; write the loader cache IceWM needs (PNG/JPEG are built
    # into libgdk_pixbuf, the rest are modules shipped by the packages).
    local loaders="${ROOTFS_DIR}/usr/lib/gdk-pixbuf-2.0/2.10.0/loaders"
    if [ -d "$loaders" ]; then
        cat > "${ROOTFS_DIR}/usr/lib/gdk-pixbuf-2.0/2.10.0/loaders.cache" <<EOF
# GdkPixbuf Image Loader Modules file
# Generated by scripts/build-minimal.sh

"/usr/lib/libgdk_pixbuf-2.0.so.0"
"png" 5 "gdk-pixbuf" "PNG" "LGPL"
"image/png" ""
"png" ""
"\211PNG\r\n\032\n" "" 100

"/usr/lib/libgdk_pixbuf-2.0.so.0"
"jpeg" 5 "gdk-pixbuf" "JPEG" "LGPL"
"image/jpeg" ""
"jpeg" "jpe" "jpg" ""
"\377\330" "" 100

EOF
        local so
        for so in libpixbufloader-gif.so libpixbufloader-bmp.so libpixbufloader_svg.so; do
            [ -f "${loaders}/${so}" ] || continue
            case "$so" in
                libpixbufloader-gif.so)
                    cat >> "${ROOTFS_DIR}/usr/lib/gdk-pixbuf-2.0/2.10.0/loaders.cache" <<EOF
"${loaders}/${so}"
"gif" 4 "gdk-pixbuf" "GIF" "LGPL"
"image/gif" ""
"gif" ""
"GIF8" "" 100

EOF
                    ;;
                libpixbufloader-bmp.so)
                    cat >> "${ROOTFS_DIR}/usr/lib/gdk-pixbuf-2.0/2.10.0/loaders.cache" <<EOF
"${loaders}/${so}"
"bmp" 5 "gdk-pixbuf" "BMP" "LGPL"
"image/bmp" "image/x-bmp" "image/x-MS-bmp" ""
"bmp" ""
"BM" "" 100

EOF
                    ;;
                libpixbufloader_svg.so)
                    cat >> "${ROOTFS_DIR}/usr/lib/gdk-pixbuf-2.0/2.10.0/loaders.cache" <<EOF
"${loaders}/${so}"
"svg" 6 "gdk-pixbuf" "Scalable Vector Graphics" "LGPL"
"image/svg+xml" "image/svg" "image/svg-xml" "image/vnd.adobe.svg+xml" "text/xml-svg" "image/svg+xml-compressed" ""
"svg" "svgz" "svg.gz" ""
" <svg" "* " 100
" <!DOCTYPE svg" "* " 100

EOF
                    ;;
            esac
        done
    fi
}

build_rootfs() {
    if [ ! -f "$STAMP" ]; then
        extract_minirootfs
        install_packages
    else
        log "Rootfs already built (${STAMP}); refreshing configuration only."
    fi
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
