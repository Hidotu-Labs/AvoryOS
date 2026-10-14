#!/usr/bin/env bash
# scripts/setup-arch.sh - Builds the AvoryOS userland tree from upstream
# Arch Linux packages (x86-64 baseline) into the disk-image rootfs.
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DISK_IMG="${ROOT_DIR}/disk.img"
BUILD_DIR="${ROOT_DIR}/build/alpine"
POPULATE_SCRIPT="${ROOT_DIR}/scripts/populate-ext2-dir.sh"

# Historical Alpine branch pins, kept as install_apk() arguments (ignored by
# the pacman backend; Arch is rolling). Do not remove: ~830 call sites pass
# a branch positionally.
ALPINE_BRANCH="v3.21"
QT6_BRANCH="v3.22"

# pacman for the target tree: upstream Arch repos only (x86-64 baseline).
# Host/CachyOS repos ship x86-64-v3/v4-tuned binaries that fault under QEMU's
# default CPU. Signatures off (https transport); no keyring needed in target.
PACMAN_CONF="${BUILD_DIR}/pacman-avory.conf"
PACMAN_CACHE="${BUILD_DIR}/paccache"
ROOTFS_DIR="${BUILD_DIR}/rootfs"

mkdir -p "${BUILD_DIR}" "${PACMAN_CACHE}" "${ROOTFS_DIR}" "${ROOTFS_DIR}/etc"

# One-time migration: a previous Alpine-based tree cannot be overlaid with
# Arch packages (different libc, different layout). Wipe it once; afterwards
# pacman tracks state itself and reruns are incremental.
if [ -f "${ROOTFS_DIR}/etc/alpine-release" ]; then
    echo "[*] Removing Alpine-based rootfs; rebuilding from Arch packages..."
    rm -rf "${ROOTFS_DIR}"
    mkdir -p "${ROOTFS_DIR}"
fi
printf '%s\n' "${ALPINE_BRANCH}" > "${ROOTFS_DIR}/etc/avoryos-alpine-branch"

# 3. Package installation via pacman (upstream Arch, x86-64 baseline).
# install_apk() keeps its Alpine-era signature (name, repo, branch) so the
# ~830 call sites below are untouched; names translate through arch-pkgs.sh
# (audited: every non-DROP result resolves) and the branch argument is
# ignored (Arch is rolling - versioned soname conflicts like the old
# ICU 74/76 split cannot occur because the whole tree builds against one
# ICU). pacman runs inside a user namespace where chown/mknod/chroot
# succeed, with files staying user-owned outside it.
# shellcheck disable=SC1091
. "${ROOT_DIR}/scripts/arch-pkgs.sh"

pacman_run() {
    mkdir -p "${ROOTFS_DIR}/var/lib/pacman"
    # Wine's Linux-only ntsync autoload helper pulls in NTSYNC-MODULE (and
    # an Arch kernel). AvoryOS uses its own kernel; omit the helper and let
    # Wine use its userspace synchronization fallback instead.
    unshare -rm pacman --config "${PACMAN_CONF}" --root "${ROOTFS_DIR}" \
        --cachedir "${PACMAN_CACHE}" --noconfirm \
        --assume-installed ntsync-autoload "$@"
}

pacman_sync_once() {
    if [ ! -f "${BUILD_DIR}/.pacman-sync" ]; then
        pacman_run -Sy
        touch "${BUILD_DIR}/.pacman-sync"
    fi
}

install_apk() {
    local ALPINE_NAME=$1
    local _REPO=$2
    local _BRANCH=${3:-"${ALPINE_BRANCH}"}
    local ARCH_NAME
    ARCH_NAME=$(arch_name "${ALPINE_NAME}")
    if [ "${ARCH_NAME}" = "DROP" ]; then
        return 0
    fi
    pacman_sync_once
    if pacman --config "${PACMAN_CONF}" --root "${ROOTFS_DIR}" -Q "${ARCH_NAME}" >/dev/null 2>&1; then
        return 0
    fi
    echo "[*] Installing package: ${ARCH_NAME} (was ${ALPINE_NAME})..."
    pacman_run -S "${ARCH_NAME}"
}

# Bootstrap the target root: filesystem (usrmerge layout, base accounts),
# glibc, bash (package scriptlets use it), busybox (/bin/sh, /sbin/init) and
# musl (loader for the prebuilt musl userland binaries like avory-login) plus
# the core tools package hooks and scriptlets call (the locale-gen hook
# already failed once on a missing `rm`).
# These come before everything else so hooks and scriptlets have a shell.
pacman_sync_once
for _bootstrap in filesystem glibc bash busybox musl coreutils sed grep gawk tar gzip xz util-linux shadow; do
    if ! pacman --config "${PACMAN_CONF}" --root "${ROOTFS_DIR}" -Q "${_bootstrap}" >/dev/null 2>&1; then
        echo "[*] Installing bootstrap package: ${_bootstrap}..."
        pacman_run -S "${_bootstrap}"
    fi
done

install_apk "st" "community"
install_apk "feh" "community"
# Arch's Wine uses WoW64: 32-bit Windows apps run with the x86-64 runtime,
# without multilib Linux packages. Include the optional .NET/HTML runtimes
# in the image so Wine does not need to download them on first use.
install_apk "wine" "community"
install_apk "wine-mono" "community"
install_apk "wine-gecko" "community"
install_apk "xorg-xrandr" "community"
install_apk "xclock" "community" "edge"
install_apk "libxft" "main"
install_apk "libxft-dev" "main"
install_apk "fontconfig" "main"
install_apk "fontconfig-dev" "main"
install_apk "libxrender" "main"
install_apk "libxrender-dev" "main"
install_apk "libexpat" "main"
install_apk "libuuid" "main"
install_apk "libpng" "main"
install_apk "freetype" "main"
install_apk "libx11" "main"
install_apk "libxcb" "main"
install_apk "libxau" "main"
install_apk "libxdmcp" "main"
install_apk "libxxf86vm" "main"
install_apk "libbsd" "main"
install_apk "libmd" "main"
install_apk "ncurses-terminfo-base" "main"
install_apk "font-liberation" "main"
install_apk "libbz2" "main"
install_apk "brotli-libs" "main"
install_apk "zlib" "main"
install_apk "libxfixes" "main"
install_apk "libxrender" "main"
install_apk "libxft" "main"
install_apk "dialog" "main"
install_apk "libcom_err" "main"
install_apk "e2fsprogs-libs" "main"
install_apk "e2fsprogs" "main"
install_apk "libsmartcols" "main"
install_apk "libfdisk" "main"
install_apk "sfdisk" "main"
install_apk "device-mapper-libs" "main"
install_apk "parted" "main"
install_apk "libgomp" "main"
install_apk "double-conversion" "community"
install_apk "libb2" "community"
install_apk "qt6-qtbase" "community" "${QT6_BRANCH}"
install_apk "qt6-qtbase-x11" "community" "${QT6_BRANCH}"
install_apk "qt6-qtwayland" "community" "${QT6_BRANCH}"
install_apk "qt6-qtsvg" "community" "${QT6_BRANCH}"
install_apk "kconfig" "community"
install_apk "kconfigwidgets" "community"
install_apk "kcoreaddons" "community"
install_apk "kcrash" "community"
install_apk "kguiaddons" "community"
install_apk "ki18n" "community"
install_apk "knotifications" "community"
install_apk "kwidgetsaddons" "community"
install_apk "kxmlgui" "community"
install_apk "kcolorscheme" "community"
install_apk "kcodecs" "community"
install_apk "kglobalaccel" "community"
install_apk "kitemviews" "community"
install_apk "kiconthemes" "community"
install_apk "libltdl" "main"
install_apk "libcanberra" "community"
install_apk "karchive" "community"
install_apk "breeze-icons" "community"
install_apk "mpfr4" "main"
install_apk "gmp" "main"
install_apk "kcalc" "community"
install_apk "fastfetch" "community"
install_apk "hwdata-pci" "main"
install_apk "icewm" "community"
install_apk "libxinerama" "main"
install_apk "openal-soft" "community"
install_apk "openal-soft-dev" "community"
install_apk "libxrandr" "main"
install_apk "libxpm" "main"
install_apk "libjpeg" "main"
install_apk "libpng" "main"
install_apk "libsm" "main"
install_apk "libice" "main"
install_apk "imlib2" "main"
install_apk "libstdc++" "main"
install_apk "libxml2" "main"
install_apk "startup-notification" "community"
install_apk "libxcomposite" "main"
install_apk "libxdamage" "main"
install_apk "xorg-server" "community"
install_apk "xf86-input-libinput" "community"
install_apk "xf86-input-evdev" "community"
install_apk "libxfont2" "community"
install_apk "libxcvt" "community"
install_apk "libfontenc" "main"
install_apk "font-cursor-misc" "main"
install_apk "font-misc-misc" "main"
install_apk "xkbcomp" "main"
install_apk "mesa-dri-gallium" "main"
install_apk "libdrm" "main"
install_apk "libdrm-tests" "main"
install_apk "mesa-gbm" "main"
install_apk "mesa-egl" "main"
install_apk "nettle" "main"
install_apk "libmagic" "main"
install_apk "libxt" "main"
install_apk "libxmu" "main"
install_apk "libxaw" "main"
install_apk "libxext" "main"
install_apk "libxkbfile" "main"
install_apk "gtk+2.0" "community"
install_apk "glib" "main"
install_apk "pango" "main"
install_apk "libatk-1.0" "main"
install_apk "gdk-pixbuf" "main"
install_apk "cairo" "main"
install_apk "fribidi" "main"
install_apk "harfbuzz" "main"
install_apk "libx11" "main"
install_apk "libxext" "main"
install_apk "libxrender" "main"
install_apk "libxi" "main"
install_apk "libxtst" "main"
install_apk "libxfixes" "main"
install_apk "libxcursor" "main"
install_apk "fontconfig" "main"
install_apk "freetype" "main"
install_apk "libpng" "main"
install_apk "libexpat" "main"
install_apk "libuuid" "main"
install_apk "shared-mime-info" "main"
install_apk "pcre2" "main"
install_apk "libffi" "main"
install_apk "libxinerama" "main"
install_apk "util-linux" "main"
install_apk "libbz2" "main"
install_apk "brotli-libs" "main"
install_apk "zlib" "main"
install_apk "lua5.4" "main"
install_apk "lua5.4-libs" "main"
install_apk "readline" "main"
install_apk "libncursesw" "main"
install_apk "htop" "main"
install_apk "gtk+3.0" "main"
install_apk "libatk-bridge-2.0" "main"
install_apk "at-spi2-core" "main"
install_apk "dbus-libs" "main"
install_apk "cairo-gobject" "main"
install_apk "libepoxy" "main"
install_apk "adwaita-icon-theme" "community"
install_apk "hicolor-icon-theme" "main"
install_apk "iso-codes" "main"
install_apk "wayland" "main"
install_apk "wayland-libs-client" "main"
install_apk "wayland-libs-server" "main"
install_apk "wayland-libs-cursor" "main"
install_apk "wayland-libs-egl" "main"
install_apk "libxkbcommon" "main"
install_apk "wlroots" "community"
install_apk "wlroots-dev" "community"
install_apk "libweston" "community"
install_apk "weston" "community"
install_apk "weston-shell-desktop" "community"
install_apk "weston-backend-drm" "community"
install_apk "weston-terminal" "community"
install_apk "wayland-protocols" "main"
install_apk "wayland-dev" "main"
install_apk "vulkan-loader" "main"
install_apk "mesa-gl" "main"
install_apk "mesa-gles" "main"
install_apk "mesa-vulkan-swrast" "main"
install_apk "mesa-demos" "community"
install_apk "freeglut" "community"
install_apk "libliftoff" "community"
install_apk "libinput" "community"
install_apk "libinput-libs" "community"
install_apk "libwacom" "community"
install_apk "libevdev" "community"
install_apk "mtdev" "community"
install_apk "libxml2" "main"
install_apk "libdisplay-info" "community"
install_apk "eudev-libs" "main"
install_apk "libgudev" "community"
install_apk "pixman" "main"
install_apk "libjpeg-turbo" "main"
install_apk "libmount" "main"
install_apk "libblkid" "main"
install_apk "libeconf" "main"
install_apk "libintl" "main"
install_apk "graphite2" "main"
install_apk "libxcomposite" "main"
install_apk "libxdamage" "main"
install_apk "gettext-libs" "main"
install_apk "libxrandr" "main"
install_apk "libseat" "community"
install_apk "libelogind" "community"
install_apk "libcap2" "main"
install_apk "libpciaccess" "main"
install_apk "gcompat" "main"
install_apk "libucontext" "main"
install_apk "libucontext-dev" "main"
install_apk "jansson" "main"
install_apk "vlc-libs" "community"
install_apk "vlc" "community"
install_apk "vlc-qt" "community"
install_apk "ffmpeg-libs" "community"
install_apk "ffmpeg4-libavcodec" "community"
install_apk "ffmpeg4-libavformat" "community"
install_apk "ffmpeg4-libavutil" "community"
install_apk "ffmpeg4-libswscale" "community"
install_apk "ffmpeg4-libpostproc" "community"
install_apk "ffmpeg4-libswresample" "community"
install_apk "libvpx" "community"
install_apk "lame-libs" "main"
install_apk "libtheora" "main"
install_apk "x264-libs" "community"
install_apk "x265-libs" "community"
install_apk "xvidcore" "community"
install_apk "libsrt" "community"
install_apk "libssh" "community"
install_apk "soxr" "community"
install_apk "libvdpau" "main"
install_apk "numactl" "main"
install_apk "libxkbcommon-x11" "main"
install_apk "libpcre2-16" "main"
install_apk "lua5.2-libs" "main"
install_apk "libmad" "community"
install_apk "libsndfile" "main"
install_apk "libogg" "main"
install_apk "libvorbis" "main"
install_apk "libflac" "main"
install_apk "opus" "main"
install_apk "taglib" "community"
install_apk "alsa-lib" "main"
install_apk "alsa-plugins" "community"
install_apk "qt5-qtbase" "community"
install_apk "qt5-qtbase-x11" "community"
install_apk "qt5-qtx11extras" "community"
install_apk "qt5-qtsvg" "community"
install_apk "qt5-qtbase-dev" "community"
install_apk "xcb-util-keysyms" "community"
install_apk "xcb-util-wm" "community"
install_apk "xcb-util-image" "community"
install_apk "xcb-util-cursor" "community"
install_apk "xcb-util-renderutil" "community"
install_apk "dbus" "main"
install_apk "dbus-x11" "main"
install_apk "dbus-daemon-launch-helper" "main"
install_apk "mesa-glapi" "main"
install_apk "llvm19-libs" "main"
install_apk "libelf" "main"
install_apk "zstd-libs" "main"
install_apk "musl-obstack" "main"
install_apk "libunwind" "main"
install_apk "libva" "main"
install_apk "xcb-util-wm" "community"
install_apk "xcb-util-image" "community"
install_apk "xcb-util-renderutil" "community"
install_apk "xcb-util" "main"
install_apk "seatd" "community"
install_apk "libxshmfence" "main"
install_apk "xcalc" "community"
install_apk "galculator" "community"
install_apk "gnome-calculator" "community"
install_apk "libadwaita" "community"
install_apk "appstream" "community"
install_apk "appstream-qt" "community"
install_apk "libxmlb" "community"
install_apk "yaml" "main"
install_apk "graphene" "main"
install_apk "gtksourceview5" "community"
install_apk "gtk4.0" "community"
install_apk "cups-libs" "main"
install_apk "harfbuzz" "main"
install_apk "harfbuzz-subset" "main"
install_apk "gsettings-desktop-schemas" "community"
install_apk "libgee" "community"
install_apk "mpfr4" "main"
install_apk "mpc1" "main"
install_apk "figlet" "community"
install_apk "nyancat" "community"
install_apk "micro-tetris" "community" "edge"
install_apk "cmus" "community"
install_apk "libflac" "main"
install_apk "alsa-lib" "main"
install_apk "faad2-libs" "community"
install_apk "libmad" "community"
install_apk "libvorbis" "main"
install_apk "wavpack-libs" "community"
install_apk "opusfile" "main"
install_apk "ffmpeg-libavcodec" "community"
install_apk "ffmpeg-libavformat" "community"
install_apk "libevent" "main"
install_apk "nspr" "main"
install_apk "nss" "main"
install_apk "pciutils-libs" "main"
install_apk "scudo-malloc" "main"
install_apk "firefox" "community"
install_apk "libasyncns" "community"
install_apk "libpulse" "community"
install_apk "libpulse-mainloop-glib" "community"
install_apk "xkeyboard-config" "main"
install_apk "setxkbmap" "community"
install_apk "font-dejavu" "main"
install_apk "sl" "community"
install_apk "gifsicle" "community"
install_apk "gtk+2.0-dev" "community"
install_apk "glib-dev" "main"
install_apk "pango-dev" "main"
install_apk "harfbuzz-dev" "main"
install_apk "graphite2-dev" "main"
install_apk "libxcomposite-dev" "main"
install_apk "libxdamage-dev" "main"
install_apk "at-spi2-core-dev" "main"
install_apk "gdk-pixbuf-dev" "main"
install_apk "libjpeg-turbo-dev" "main"
install_apk "util-linux-dev" "main"
install_apk "libeconf-dev" "main"
install_apk "gettext-dev" "main"
install_apk "libxrandr-dev" "main"
install_apk "libxinerama-dev" "main"
install_apk "cairo-dev" "main"
install_apk "libx11-dev" "main"
install_apk "libxrender-dev" "main"
install_apk "fontconfig-dev" "main"
install_apk "freetype-dev" "main"
install_apk "libpng-dev" "main"
install_apk "zlib-dev" "main"
install_apk "libxext-dev" "main"
install_apk "libxi-dev" "main"
install_apk "libxcursor-dev" "main"
install_apk "libxfixes-dev" "main"
install_apk "xorgproto" "main"
install_apk "python3" "main"
install_apk "dbus-dev" "main"
install_apk "dbus" "main"
install_apk "nano" "main"
install_apk "make" "main"
install_apk "gcc" "main"
install_apk "isl26" "main"
install_apk "mpfr4" "main"
install_apk "mpc1" "main"
install_apk "musl-dev" "main"
install_apk "binutils" "main"
install_apk "gtk+3.0-dev" "main"
install_apk "at-spi2-core-dev" "main"
install_apk "libepoxy-dev" "main"
install_apk "wayland-dev" "main"
install_apk "libxkbcommon-dev" "main"
install_apk "libxcb-dev" "main"
install_apk "xcb-util-wm-dev" "community"
install_apk "xcb-util-image-dev" "community"
install_apk "xcb-util-renderutil-dev" "community"
install_apk "pixman-dev" "main"
install_apk "libdrm-dev" "main"
install_apk "libinput-dev" "community"
install_apk "libseat-dev" "community"
install_apk "vulkan-loader-dev" "main"
install_apk "mesa-dev" "main"
install_apk "mesa" "main"
install_apk "xfce4" "community"
install_apk "xfce4-session" "community"
install_apk "xfwm4" "community"
install_apk "xfdesktop" "community"
install_apk "xfce4-panel" "community"
install_apk "xfce4-settings" "community"
install_apk "lz4-libs" "main"
install_apk "vte3" "community"
install_apk "xfce4-terminal" "community"
install_apk "xfconf" "community"
install_apk "libxklavier" "community"
install_apk "libxfce4util" "community"
install_apk "libxfce4ui" "community"
install_apk "libgtop" "community"
install_apk "garcon" "community"
install_apk "exo" "community"
install_apk "exo-libs" "community"
install_apk "libxfce4panel" "community"
install_apk "thunar" "community"
install_apk "thunar-volman" "community"
install_apk "tumbler" "community"
install_apk "xfce4-appfinder" "community"
install_apk "xfce4-power-manager" "community"
install_apk "xfce4-notifyd" "community"
install_apk "xfce4-screensaver" "community"
install_apk "linux-pam" "main"
install_apk "acl-libs" "main"
install_apk "elogind-common" "community"
install_apk "elogind" "community"
install_apk "lightdm" "community"
install_apk "lightdm-gtk-greeter" "community"
install_apk "lxdm" "community"
install_apk "xinit" "community"
install_apk "mcookie" "main"
install_apk "xauth" "community"
install_apk "iceauth" "community"
install_apk "xmodmap" "community"
install_apk "xsetroot" "community"
install_apk "xfce4-panel-dev" "community"
install_apk "xfce4-battery-plugin" "community"
install_apk "xfce4-clipman-plugin" "community" "edge"
install_apk "xfce4-systemload-plugin" "community" "edge"
install_apk "xfce4-whiskermenu-plugin" "community"
install_apk "libnotify" "community"
install_apk "notification-daemon" "testing" "edge"
install_apk "polkit" "community"
install_apk "polkit-elogind" "community"
install_apk "upower" "community"
install_apk "udisks2" "community"
install_apk "gcr" "community"
install_apk "gcr4" "community"
install_apk "gcr4-base" "community"
install_apk "libsecret" "main"
install_apk "libgcrypt" "main"
install_apk "libgpg-error" "main"
install_apk "p11-kit" "main"
install_apk "libtasn1" "main"
install_apk "libxres" "community"
install_apk "libxpresent" "community"
install_apk "libwnck3" "community"
install_apk "xrdb" "community"
install_apk "xhost" "community"
install_apk "libpng" "main"
install_apk "librsvg" "community"
install_apk "shared-mime-info" "main"
install_apk "adwaita-icon-theme" "community"
install_apk "gsettings-desktop-schemas" "community"
install_apk "gettext-libs" "main"
install_apk "gtksourceview" "community"
install_apk "mousepad" "community"
install_apk "gspell" "community"
install_apk "libxfce4ui" "community"
install_apk "menu-cache" "community"
install_apk "pcmanfm" "community"
install_apk "libfm" "community"
install_apk "libfm-extra" "community"
install_apk "libexif" "community"
install_apk "tumbler" "community"
install_apk "gvfs" "community"
install_apk "gvfs-archive" "community"
install_apk "gvfs-fuse" "community"
install_apk "file" "main"
install_apk "sdl2" "community"
install_apk "sdl2-dev" "community"
install_apk "sdl2_image" "community"
install_apk "sdl2_image-dev" "community"
install_apk "sdl2_ttf" "community"
install_apk "sdl2_ttf-dev" "community"
install_apk "sdl2_mixer" "community"
install_apk "sdl2_mixer-dev" "community"
install_apk "sdl2_net" "community"
install_apk "sdl2_net-dev" "community"
install_apk "sdl3" "community" "edge"
install_apk "sdl3-dev" "community" "edge"
install_apk "sdl3_ttf" "community" "edge"
install_apk "sdl3_ttf-dev" "community" "edge"
install_apk "libplacebo" "community" "edge"
install_apk "libplacebo-dev" "community" "edge"
install_apk "capstone" "community" "edge"
install_apk "capstone-dev" "community" "edge"
install_apk "bubblewrap" "main"
install_apk "xdg-dbus-proxy" "community"
install_apk "gnome-keyring" "community"
install_apk "libavif" "main"
install_apk "libatomic" "main"
install_apk "aom-libs" "main"
install_apk "enchant2-libs" "community"
install_apk "flite" "main"
install_apk "gstreamer" "main"
install_apk "gst-plugins-base" "main"
install_apk "orc" "main"
install_apk "vo-aacenc" "community"
install_apk "vo-amrwbenc" "community"
install_apk "libnice" "community"
install_apk "libzbar" "community"
install_apk "libxv" "main"
install_apk "libusb" "main"
install_apk "libsrtp" "main"
install_apk "tiff" "main"
install_apk "spandsp" "main"
install_apk "gst-plugins-bad" "community"
install_apk "fdk-aac" "community"
install_apk "mpg123-libs" "main"
install_apk "speex" "main"
install_apk "gst-plugins-good" "community"
install_apk "gst-plugins-ugly" "community"
install_apk "gst-libav" "community"
install_apk "ffmpeg-libavutil" "community"
install_apk "ffmpeg-libavfilter" "community"
install_apk "ffmpeg-libswscale" "community"
install_apk "ffmpeg-libswresample" "community"
install_apk "ffmpeg-libpostproc" "community"
install_apk "rav1e-libs" "community"
install_apk "libSvtAv1Enc" "community"
install_apk "onevpl-libs" "community"
install_apk "libopenmpt" "community"
install_apk "libbluray" "community"
install_apk "librist" "community"
install_apk "mbedtls" "main"
install_apk "cjson" "main"
install_apk "libzmq" "main"
install_apk "libsodium" "main"
install_apk "libplacebo" "community"
install_apk "glslang-libs" "main"
install_apk "shaderc" "community"
install_apk "spirv-tools" "main"
install_apk "libdovi" "community"
install_apk "lilv-libs" "community"
install_apk "serd-libs" "community"
install_apk "sord-libs" "community"
install_apk "sratom" "community"
install_apk "zix-libs" "community"
install_apk "zimg" "community"
install_apk "vidstab" "community"
install_apk "libde265" "main"
install_apk "openh264" "community"
install_apk "libass" "community"
install_apk "libunibreak" "community"
install_apk "a52dec" "community"
install_apk "openjpeg" "main"
install_apk "opencore-amr" "community"
install_apk "gsm" "main"
install_apk "faac" "community"
install_apk "libmodplug" "community"
install_apk "soundtouch" "community"
install_apk "libtag" "community"
install_apk "sbc" "community"
install_apk "libldac" "community"
install_apk "libfreeaptx" "community"
install_apk "libsamplerate" "main"
install_apk "webrtc-audio-processing-1" "community"
install_apk "libcamera" "community"
install_apk "libcamera-ipa" "community"
install_apk "enchant2" "community"
install_apk "enchant2-aspell" "community"
install_apk "enchant2-hunspell" "community"
install_apk "enchant2-nuspell" "community"
install_apk "enchant2-data" "community"
install_apk "aspell-libs" "main"
install_apk "libhunspell" "main"
install_apk "nuspell-libs" "community"
install_apk "libdc1394" "community"
install_apk "directfb" "community"
install_apk "tslib" "community"
install_apk "neon" "main"
install_apk "openexr-libopenexr" "community"
install_apk "openexr-libiex" "community"
install_apk "openexr-libilmthread" "community"
install_apk "openexr-libopenexrcore" "community"
install_apk "imath" "community"
install_apk "harfbuzz-icu" "main"
install_apk "hyphen" "community"
install_apk "icu-data-en" "main"
install_apk "icu-libs" "main"
install_apk "libjxl" "community"
install_apk "libhwy" "community"
install_apk "libmanette" "community"
install_apk "libseccomp" "main"
install_apk "libsoup3" "community"
install_apk "gmp" "main"
install_apk "gnutls" "main"
install_apk "duktape" "community"
install_apk "libproxy" "community"
install_apk "ca-certificates" "main"
install_apk "glib-networking" "community"
install_apk "sqlite-libs" "main"
install_apk "libwoff2common" "community"
install_apk "libwoff2dec" "community"
install_apk "libwoff2enc" "community"
install_apk "libwebpmux" "main"
install_apk "libwebpdemux" "main"
install_apk "webkit2gtk-4.1" "community"
install_apk "badwolf" "community"
install_apk "font-noto" "community"
install_apk "font-noto-emoji" "community"
install_apk "musl-locales" "main"
install_apk "tzdata" "main"
install_apk "alacritty" "community"
install_apk "netsurf" "community" "edge"
install_apk "duktape" "community" "edge"
install_apk "zstd-libs" "main" "edge"
install_apk "xz-libs" "main" "edge"
install_apk "curl" "main" "edge"
install_apk "libcurl" "main" "edge"
install_apk "libsharpyuv" "main" "edge"
install_apk "libwebp" "main" "edge"
install_apk "lcms2" "main" "edge"
install_apk "libgcc" "main" "edge"
install_apk "libdav1d" "main" "edge"
install_apk "dav1d" "main" "edge"
install_apk "libxml2" "main" "edge"
install_apk "libxslt" "main" "edge"
install_apk "librsvg" "community" "edge"
install_apk "openssl" "main" "edge"
install_apk "libssl3" "main" "edge"
install_apk "libcrypto3" "main" "edge"
install_apk "nghttp2-libs" "main" "edge"
install_apk "libidn2" "main" "edge"
install_apk "libunistring" "main" "edge"
install_apk "libpsl" "main" "edge"
install_apk "c-ares" "main" "edge"
install_apk "brotli-libs" "main" "edge"
install_apk "ca-certificates" "main"
install_apk "libbz2" "main"
install_apk "zlib" "main"
install_apk "libtirpc-nokrb" "main"
install_apk "xwayland" "community" 
install_apk "weston-xwayland" "community" 
install_apk "cmatrix" "community" 
install_apk "btop" "community" 
install_apk "kauth" "community"
install_apk "kbookmarks" "community"
install_apk "kcmutils" "community"
install_apk "kcompletion" "community"
install_apk "kcontacts" "community"
install_apk "kdnssd" "community"
install_apk "kglobalaccel" "community"
install_apk "kholidays" "community"
install_apk "kidletime" "community"
install_apk "kjobwidgets" "community"
install_apk "kio" "community"
install_apk "kio-extras" "community"
# Optional KIO helpers are not installed by pacman's dependency resolver:
# admin:// access, AppImage thumbnails and the info:// worker's Perl runtime.
# These extend file browsing; root's malformed desktop URL is handled by
# configure-accounts.sh through normalized XDG user-directory paths.
install_apk "kio-admin" "community"
install_apk "libappimage" "community"
install_apk "perl" "main"
install_apk "knewstuff" "community"
install_apk "kpackage" "community"
install_apk "kparts" "community"
install_apk "kpeople" "community"
install_apk "krunner" "community"
install_apk "kservice" "community"
install_apk "ktextwidgets" "community"
install_apk "kwallet" "community"
install_apk "kquickcharts" "community"
install_apk "kquickcharts-dev" "community"
install_apk "kwalletmanager" "community"
install_apk "kwindowsystem" "community"
install_apk "solid-libs" "community"
install_apk "libplist" "community"
install_apk "libimobiledevice-glue" "community"
install_apk "libusbmuxd" "community"
install_apk "libimobiledevice" "community"
install_apk "kstatusnotifieritem" "community"
install_apk "networkmanager" "community"
install_apk "networkmanager-wifi" "community"
install_apk "networkmanager-tui" "community"
install_apk "networkmanager-qt" "community"
install_apk "modemmanager" "community"
install_apk "modemmanager-qt" "community"
install_apk "mobile-broadband-provider-info" "community"
install_apk "kdecoration" "community"
install_apk "layer-shell-qt" "community"
install_apk "libkscreen" "community"
install_apk "libksysguard" "community"
install_apk "lm-sensors-libs" "main"
install_apk "libnl3" "main"
install_apk "libpcap" "main"
install_apk "plasma5support" "community"
install_apk "plasma-activities" "community"
install_apk "plasma-activities-libs" "community"
install_apk "plasma-activities-stats" "community"
install_apk "ksvg" "community"
install_apk "kwayland" "community"
install_apk "kglobalacceld" "community"
install_apk "kscreenlocker" "community"
install_apk "qt6-qtsensors" "community" "${QT6_BRANCH}"
install_apk "libqaccessibilityclient" "community"
install_apk "breeze-cursors" "community"
install_apk "breeze" "community"
install_apk "frameworkintegration" "community"
install_apk "qqc2-desktop-style" "community"
install_apk "qqc2-breeze-style" "community"
install_apk "kirigami-libs" "community"
install_apk "kirigami" "community"
install_apk "kirigami-addons" "community"
install_apk "pipewire" "community"
install_apk "kpipewire" "community"
install_apk "pipewire-libs" "community"
install_apk "pipewire-pulse" "community"
install_apk "pipewire-alsa" "community"
install_apk "pipewire-tools" "community"
install_apk "pipewire-openrc" "community" || true
install_apk "pipewire-pulse-openrc" "community" || true
install_apk "wireplumber" "community"
install_apk "wireplumber-libs" "community"
# This VM has no camera/Bluetooth/MIDI/seat hardware. Upstream wireplumber
# otherwise probes the libcamera monitor and aborts inside it at startup
# (SIGABRT in wireplumber, see serial log), and rescans missing SPA monitors
# in a hot loop. Disabling those profiles matches upstream VM guidance.
mkdir -p "${ROOTFS_DIR}/etc/wireplumber/wireplumber.conf.d"
cat > "${ROOTFS_DIR}/etc/wireplumber/wireplumber.conf.d/10-avoryos-vm.conf" << 'EOF'
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
install_apk "pulseaudio-utils" "community"
install_apk "pulseaudio-qt" "community"
install_apk "avahi-libs" "main"
install_apk "bluez" "main"
install_apk "bluez-libs" "main"
install_apk "qt6-qtshadertools" "community" "${QT6_BRANCH}"
install_apk "icu-data-full" "main"
# Qt 6.8.3 links ICU 76 while v3.21 ships ICU 74: install the v3.22 runtime
# alongside it. Branch-keyed markers let both generations coexist on disk,
# which is exactly what a package solver cannot express (one icu-libs).
install_apk "icu-libs" "main" "${QT6_BRANCH}"
install_apk "icu-data-full" "main" "${QT6_BRANCH}"
install_apk "qt6-qt5compat" "community" "${QT6_BRANCH}"
install_apk "kwin" "community"
install_apk "plasma-workspace" "community"
install_apk "plasma-workspace-libs" "community"
install_apk "plasma-workspace-wallpapers" "community"
install_apk "plasma-workspace-x11" "community"
install_apk "plasma-desktop" "community"
# Plasma 6's notification server, tray applet and settings module ship in
# plasma-workspace (org.kde.plasma.Notifications.service -> plasma_waitforname,
# owned by plasmashell at session start). knotifications and libnotify
# (notify-send) are installed above; knotifyconfig supplies KDE app
# notification settings. Keep sounds explicit too, since install_apk skips
# packages already present in the tree.
install_apk "knotifyconfig" "community"
install_apk "ocean-sound-theme" "community"
install_apk "sound-theme-freedesktop" "community"
# xfce4-notifyd (installed above for XFCE sessions) ships a competing
# org.freedesktop.Notifications D-Bus activator. With both files present the
# bus races and Plasma reports "Notification service not available /
# provided by Xfce Notify Daemon". Mask the XFCE activator so D-Bus
# activation resolves to plasma_waitforname and plasmashell owns the name.
# XFCE sessions are unaffected: xfce4-notifyd still autostarts there via
# /etc/xdg/autostart/xfce4-notifyd.desktop (OnlyShowIn=XFCE) and owns the
# name by running, without needing D-Bus activation.
rm -f "${ROOTFS_DIR}/usr/share/dbus-1/services/org.xfce.xfce4-notifyd.Notifications.service"
# Optional Plasma applications/backends are not pulled in by plasma-desktop.
# Discover needs PackageKit's Qt6 backend for the Arch repository view;
# pacman resolves its daemon, libalpm, polkit and Qt dependencies together.
install_apk "discover" "community"
install_apk "packagekit-qt6" "community"
install_apk "archlinux-appstream-data" "community"
install_apk "archlinux-keyring" "main"
install_apk "bluedevil" "community"
install_apk "bluez-qt" "community"
install_apk "drkonqi" "community"
install_apk "kinfocenter" "community"
install_apk "kscreen" "community"
install_apk "milou" "community"
install_apk "plasma-disks" "community"
install_apk "plasma-firewall" "community"
install_apk "plasma-integration" "community"
install_apk "plasma-nm" "community"
install_apk "plasma-pa" "community"
install_apk "plasma-systemmonitor" "community"
install_apk "plasma-vault" "community"
install_apk "plasma-welcome" "community"
install_apk "powerdevil" "community"
install_apk "systemsettings" "community"
install_apk "kdeplasma-addons" "community"
install_apk "libplasma" "community"
install_apk "musl-fts" "main"
install_apk "lmdb" "main"
install_apk "dolphin" "community"
install_apk "attica" "community"
install_apk "syndication" "community"
install_apk "konsole" "community"
install_apk "syntax-highlighting" "community"
install_apk "ktexteditor" "community"
install_apk "kuserfeedback" "community"
install_apk "kate-common" "community"
install_apk "kate" "community"
install_apk "kdbusaddons" "community"
install_apk "kfind" "community"
install_apk "kfilemetadata" "community"
install_apk "libarchive" "main"
install_apk "libzip" "community"
install_apk "lrzip" "community"
install_apk "unzip" "main"
install_apk "zip" "main"
install_apk "zstd" "main"
install_apk "ark" "community"
install_apk "okular-common" "community"
install_apk "phonon-qt6" "community"
install_apk "purpose" "community"
install_apk "qt6-qtspeech" "community" "${QT6_BRANCH}"
install_apk "threadweaver" "community"
install_apk "libdjvulibre" "community"
install_apk "poppler" "main"
install_apk "poppler-qt6" "community"
install_apk "libspectre" "community"
install_apk "kpty" "community"
install_apk "libutempter" "main"
install_apk "kdegraphics-mobipocket" "community"
install_apk "libqca-qt6" "community"
install_apk "discount" "community"
install_apk "hunspell" "main"
install_apk "sonnet" "community"
install_apk "okular" "community"
install_apk "libquadmath" "main"
install_apk "libgfortran" "main"
install_apk "openblas" "community"
install_apk "hwloc" "main"
install_apk "onetbb" "community"
install_apk "libopencv_core" "community"
install_apk "libopencv_imgproc" "community"
install_apk "spectacle" "community"
install_apk "exiv2" "community"
install_apk "libraw" "community"
install_apk "libkdcraw" "community"
install_apk "kcolorpicker" "community"
install_apk "kimageannotator" "community"
install_apk "kitemmodels" "community"
install_apk "lcms2" "main"
install_apk "kimageformats" "community"
install_apk "gwenview" "community"
install_apk "kdeconnect" "community"
install_apk "sddm" "community"
install_apk "sddm-breeze" "community"
install_apk "qt6-qtdeclarative" "community" "${QT6_BRANCH}"
install_apk "qt6-qtpositioning" "community" "${QT6_BRANCH}"
install_apk "qt6-qtimageformats" "community" "${QT6_BRANCH}"
install_apk "qt6-qtmultimedia" "community" "${QT6_BRANCH}"
install_apk "qt6-qtnetworkauth" "community" "${QT6_BRANCH}"
install_apk "qt6-qtvirtualkeyboard" "community" "${QT6_BRANCH}"
install_apk "qt6-qtconnectivity" "community" "${QT6_BRANCH}"
install_apk "qt6-qttools" "community" "${QT6_BRANCH}"
install_apk "baloo" "community"
install_apk "baloo-widgets" "community"
install_apk "kded" "community"
install_apk "kdialog" "community"
install_apk "kde-cli-tools" "community"
install_apk "kde-gtk-config" "community"
install_apk "kmenuedit" "community"
install_apk "ksystemlog" "community"
install_apk "ksystemstats" "community"
install_apk "kactivitymanagerd" "community"
install_apk "qt6-qtbase-sqlite" "community" "${QT6_BRANCH}"
install_apk "appstream-qt" "community"
install_apk "libkexiv2" "community"
install_apk "kunitconversion" "community"
install_apk "qcoro" "community"
install_apk "libnm" "community"
install_apk "polkit-qt6" "community"
install_apk "kdeclarative" "community"
install_apk "oxygen" "community"
install_apk "xdg-utils" "community"
install_apk "xdg-user-dirs" "community"
install_apk "desktop-file-utils" "community"
install_apk "libdmtx-libs" "community"
install_apk "libdmtx" "community"
install_apk "zxing-cpp" "community"
install_apk "libqrencode" "community"
install_apk "prison" "community"
install_apk "json-glib" "community"
install_apk "xdg-desktop-portal" "community"
install_apk "xdg-desktop-portal-kde" "community"
install_apk "xdg-desktop-portal-gtk" "community"
install_apk "plasma-browser-integration" "community"
install_apk "polkit-kde-agent-1" "community"
install_apk "accountsservice" "community"
install_apk "ksshaskpass" "community"
install_apk "fuse3-libs" "main"
install_apk "kio-fuse" "community"
install_apk "kdoctools" "community"
install_apk "kdesu" "community"
install_apk "kdsoap-qt6" "community"
install_apk "kdsoap-ws-discovery-client" "community"
install_apk "talloc" "main" || true
install_apk "tevent" "main" || true
install_apk "samba-libs" "main" || true
install_apk "libsmbclient" "main" || true
install_apk "openrc" "main"
install_apk "openrc-init" "main"
install_apk "dbus-openrc" "main"
install_apk "plymouth" "community" || true
install_apk "plymouth-themes" "community" || true
install_apk "plymouth-drm" "community" || true
install_apk "plymouth-openrc-plugin" "community" "edge" || true

# User accounts, DRI symlinks, session wrappers (startplasma-x11, kwin).
# configure-accounts.sh is package-manager agnostic (operates on files).
echo "[*] Configuring accounts and session wrappers..."
"${ROOT_DIR}/scripts/configure-accounts.sh" "${ROOTFS_DIR}"

# --- Minimal boot contract (busybox init, no OpenRC on Arch) ---
# /sbin/init is busybox; it runs /etc/init.d/rcS once, then respawns the
# console login. No service manager, no dependency graph to satisfy.
mkdir -p "${ROOTFS_DIR}/etc/init.d"
cat <<'EOF' > "${ROOTFS_DIR}/etc/init.d/rcS"
#!/bin/sh
# AvoryOS boot (busybox init rcS; OpenRC is unavailable on Arch official
# repos, so this script is the service manager: fsck -> dbus -> pipewire,
# each guarded so a missing binary or empty config never fails the boot.
# Status lines mimic OpenRC's ` * Starting foo ... [ ok ]` format (plain
# text, no ANSI colors, so the serial log stays clean).
ebegin() { printf ' * %s ...' "$*"; }
eend() { if [ "${1:-0}" -eq 0 ] 2>/dev/null; then printf ' [ ok ]\n'; else printf ' [ !! ]\n'; fi; return 0; }
eskip() { printf ' [ skipped ]\n'; return 0; }

ebegin "Mounting pseudo-filesystems"
mount -t proc proc /proc 2>/dev/null; rc1=$?
mount -t sysfs sysfs /sys 2>/dev/null; rc2=$?
mount -t devtmpfs dev /dev 2>/dev/null || true
mkdir -p /run /run/lock /run/mount /var/log /tmp 2>/dev/null
chmod 1777 /tmp 2>/dev/null || true
if [ "$rc1" -eq 0 ] && [ "$rc2" -eq 0 ]; then eend 0; else eend 1; fi

ebegin "Setting hostname"
hostname AvoryOS 2>/dev/null; eend $?

if [ -x /sbin/avory-early-init ]; then
    ebegin "Running early init"
    /sbin/avory-early-init 2>/dev/null; eend $?
fi
if [ ! -s /etc/machine-id ]; then
    echo "b08040a9e7114fea9634b7f94da7e722" > /etc/machine-id
fi
ebegin "Checking filesystems"
if [ -x /sbin/fsck ] && grep -q -v '^[[:space:]]*\(#\|$\)' /etc/fstab 2>/dev/null; then
    fsck -A -T -a >>/var/log/boot.log 2>&1; eend $?
else
    eskip
fi
mkdir -p /var/lib/dbus /run/dbus
[ -e /run/mount/utab ] || : > /run/mount/utab 2>/dev/null || true
[ -e /var/lib/dbus/machine-id ] || cp -f /etc/machine-id /var/lib/dbus/machine-id 2>/dev/null || true
command -v dbus-uuidgen >/dev/null 2>&1 && dbus-uuidgen --ensure 2>/dev/null || true
ebegin "Starting system message bus"
if command -v dbus-daemon >/dev/null 2>&1 && [ ! -S /run/dbus/system_bus_socket ]; then
    dbus-daemon --system --fork 2>/dev/null || true
fi
# Wait for the system bus so dependents (pipewire, logins) do not race it.
for i in 1 2 3 4 5 6 7 8 9 10; do
    [ -S /run/dbus/system_bus_socket ] && break
    sleep 0.5
done
if [ -S /run/dbus/system_bus_socket ]; then eend 0; else eend 1; fi
# --- pipewire system instance (root-owned; user sessions start their own
# copy in the startplasma-x11 wrapper if none is listening) ---
ebegin "Starting PipeWire"
PIPE_OK=1
if command -v pipewire >/dev/null 2>&1; then
    PIPE_RUNTIME=/run/pipewire
    mkdir -p "$PIPE_RUNTIME" 2>/dev/null || true
    chmod 0755 "$PIPE_RUNTIME" 2>/dev/null || true
    if [ ! -S "$PIPE_RUNTIME/pipewire-0" ]; then
        XDG_RUNTIME_DIR="$PIPE_RUNTIME" nohup pipewire >>/var/log/pipewire.log 2>&1 &
        for i in 1 2 3 4 5 6 7 8 9 10; do
            [ -S "$PIPE_RUNTIME/pipewire-0" ] && break
            sleep 0.5
        done
    fi
    [ -S "$PIPE_RUNTIME/pipewire-0" ] && PIPE_OK=0
    eend "$PIPE_OK"
    if [ "$PIPE_OK" -eq 0 ]; then
        ebegin "Starting WirePlumber"
        if command -v wireplumber >/dev/null 2>&1; then
            if command -v pidof >/dev/null 2>&1; then
                # Keep the probe synchronous; background only the daemon,
                # not an extra shell wrapping the entire `||` list.
                if ! pidof wireplumber >/dev/null 2>&1; then
                    XDG_RUNTIME_DIR="$PIPE_RUNTIME" nohup wireplumber >>/var/log/pipewire.log 2>&1 &
                fi
            else
                XDG_RUNTIME_DIR="$PIPE_RUNTIME" nohup wireplumber >>/var/log/pipewire.log 2>&1 &
            fi
            eend 0
        else
            eskip
        fi
        ebegin "Starting PipeWire PulseAudio bridge"
        if command -v pipewire-pulse >/dev/null 2>&1; then
            if [ ! -S "$PIPE_RUNTIME/pipewire-pulse" ]; then
                XDG_RUNTIME_DIR="$PIPE_RUNTIME" nohup pipewire-pulse >>/var/log/pipewire.log 2>&1 &
            fi
            eend 0
        else
            eskip
        fi
    fi
    unset PIPE_RUNTIME
else
    eskip
fi
echo " * Entering runlevel: default"
EOF
chmod +x "${ROOTFS_DIR}/etc/init.d/rcS"

# Early init: seed /run/udev/data so libudev sees the already-initialized
# display and input hardware (seat/uaccess tags for Xorg and libinput).
cat <<'EOF' > "${ROOTFS_DIR}/sbin/avory-early-init"
#!/bin/sh
# Early initialization for AvoryOS before OpenRC
mkdir -p /run/udev/data /run/plymouth /var/log

# Tell libudev that display and input hardware is already initialized
cat <<'UDEV' > /run/udev/data/c226:0
E:DEVNAME=/dev/dri/card0
E:SUBSYSTEM=drm
E:MAJOR=226
E:MINOR=0
TAGS=:seat:uaccess:
UDEV
cat <<'UDEV' > /run/udev/data/c226:2
E:DEVNAME=/dev/dri/card2
E:SUBSYSTEM=drm
E:MAJOR=226
E:MINOR=2
TAGS=:seat:uaccess:
UDEV
cat <<'UDEV' > /run/udev/data/c226:128
E:DEVNAME=/dev/dri/renderD128
E:SUBSYSTEM=drm
E:MAJOR=226
E:MINOR=128
TAGS=:seat:uaccess:
UDEV
cat <<'UDEV' > /run/udev/data/c226:129
E:DEVNAME=/dev/dri/renderD129
E:SUBSYSTEM=drm
E:MAJOR=226
E:MINOR=129
TAGS=:seat:uaccess:
UDEV
cat <<'UDEV' > /run/udev/data/c29:0
E:DEVNAME=/dev/fb0
E:SUBSYSTEM=graphics
E:MAJOR=29
E:MINOR=0
TAGS=:seat:uaccess:
UDEV
cat <<'UDEV' > /run/udev/data/c13:64
E:DEVNAME=/dev/input/event0
E:SUBSYSTEM=input
E:MAJOR=13
E:MINOR=64
TAGS=:seat:uaccess:
UDEV
cat <<'UDEV' > /run/udev/data/c13:65
E:DEVNAME=/dev/input/event1
E:SUBSYSTEM=input
E:MAJOR=13
E:MINOR=65
TAGS=:seat:uaccess:
UDEV

# Select the Xorg profile for this boot's GPU topology before any display
# manager runs: an image baked for VFIO otherwise leaves Xorg pointed at a
# nonexistent card2 on plain `make run` boots and the server dies instantly.
if [ -x /usr/bin/avory-xorg-profile ]; then
    /usr/bin/avory-xorg-profile
fi
EOF
chmod +x "${ROOTFS_DIR}/sbin/avory-early-init"

# Console login binary (inittab respawns it on tty1).
if [ -f "${ROOT_DIR}/userland/avory-login.elf" ]; then
    cp "${ROOT_DIR}/userland/avory-login.elf" "${ROOTFS_DIR}/bin/avory-login"
    chmod +x "${ROOTFS_DIR}/bin/avory-login"
fi

# Both Xorg profiles ship in the image; /sbin/avory-early-init selects the
# one matching the boot's PCI topology (amdgpu passthrough vs native
# card0), so bare `sddm` works with or without VFIO. The default copy is
# the safe native layout; the disk-image step reapplies the build-time
# XORG_PROFILE choice for boots where detection is inconclusive.
mkdir -p "${ROOTFS_DIR}/etc/X11/xorg.conf.d" "${ROOTFS_DIR}/usr/share/avory-xorg"
install -m 0644 "${ROOT_DIR}/config/xorg/10-modesetting.conf" \
    "${ROOTFS_DIR}/usr/share/avory-xorg/10-modesetting-amdgpu.conf"
install -m 0644 "${ROOT_DIR}/config/xorg/10-native.conf" \
    "${ROOTFS_DIR}/usr/share/avory-xorg/10-modesetting-native.conf"
install -m 0644 "${ROOT_DIR}/config/xorg/10-native.conf" \
    "${ROOTFS_DIR}/etc/X11/xorg.conf.d/10-modesetting.conf"
install -m 0755 "${ROOT_DIR}/config/xorg/avory-xorg-profile.sh" \
    "${ROOTFS_DIR}/usr/bin/avory-xorg-profile"
# SDDM display manager defaults, baked into the image so bare `sddm`
# behaves like the /bin/startx.sh path (which regenerates the same files
# at boot). Without /etc/sddm.conf SDDM hides uid<1000 (no root login);
# without permissive PAM the stock Arch sddm* services can fail the
# greeter session on AvoryOS (no systemd/logind, no shadow).
mkdir -p "${ROOTFS_DIR}/etc/sddm.conf.d" "${ROOTFS_DIR}/etc/pam.d"
install -m 0644 "${ROOT_DIR}/config/xorg/20-output.conf" \
    "${ROOTFS_DIR}/etc/sddm.conf.d/20-output.conf"
install -m 0755 "${ROOT_DIR}/config/xorg/avory-prime-display.sh" \
    "${ROOTFS_DIR}/usr/bin/avory-prime-display"
mkdir -p "${ROOTFS_DIR}/etc/X11/xinit/xinitrc.d"
install -m 0755 "${ROOT_DIR}/config/xorg/45-prime-output.sh" \
    "${ROOTFS_DIR}/etc/X11/xinit/xinitrc.d/45-prime-output.sh"
cat > "${ROOTFS_DIR}/etc/sddm.conf" <<'EOF'
[General]
DisplayServer=x11
HaltCommand=/sbin/poweroff
RebootCommand=/sbin/reboot
Numlock=none

[Theme]
Current=breeze
ThemeDir=/usr/share/sddm/themes
FacesDir=/usr/share/sddm/faces
CursorTheme=Adwaita

[X11]
ServerPath=/usr/bin/Xorg
ServerArguments=-noreset -nolisten tcp -ac
SessionCommand=/usr/share/sddm/scripts/Xsession
SessionDir=/usr/share/xsessions
DisplayCommand=/usr/bin/avory-prime-display
DisplayStopCommand=/usr/share/sddm/scripts/Xstop
XauthPath=/usr/bin/xauth
EnableHiDPI=true

[Users]
DefaultPath=/usr/bin:/bin:/usr/local/bin:/opt/coreutils/bin:/opt/bash/bin
MinimumUid=0
MaximumUid=60000
HideUsers=
RememberLastUser=true
RememberLastSession=true

[Last]
Session=plasmax11.desktop
User=root
EOF
cat > "${ROOTFS_DIR}/etc/pam.d/sddm-greeter" <<'EOF'
#%PAM-1.0
auth      required  pam_env.so
auth      required  pam_permit.so
account   required  pam_permit.so
password  required  pam_deny.so
session   required  pam_unix.so
-session  optional  pam_elogind.so
EOF
cat > "${ROOTFS_DIR}/etc/pam.d/sddm" <<'EOF'
#%PAM-1.0
auth      required  pam_permit.so
account   required  pam_permit.so
password  required  pam_deny.so
session   required  pam_env.so
session   required  pam_unix.so
-session  optional  pam_limits.so
-session  optional  pam_elogind.so
EOF
cat > "${ROOTFS_DIR}/etc/pam.d/sddm-autologin" <<'EOF'
#%PAM-1.0
auth      required  pam_permit.so
account   required  pam_permit.so
password  required  pam_deny.so
session   required  pam_env.so
session   required  pam_unix.so
-session  optional  pam_limits.so
-session  optional  pam_elogind.so
EOF
# Inittab for busybox init: one rcS pass, console login, sane shutdown.
# /sbin/init is busybox (symlinked below; Arch ships no init of its own here).
ln -sf /usr/bin/busybox "${ROOTFS_DIR}/sbin/init" 2>/dev/null || true
cat <<'EOF' > "${ROOTFS_DIR}/etc/inittab"
# /etc/inittab for AvoryOS (busybox init)

::sysinit:/etc/init.d/rcS

# Console login on tty1
tty1::respawn:/bin/avory-login

# Shutdown & reboot
::shutdown:/bin/umount -a -r 2>/dev/null
::ctrlaltdel:/sbin/reboot
EOF
# /etc/mtab -> /proc/self/mounts: df, mount, libmount and Qt's QStorageInfo
# fall back to it when mountinfo is unavailable; without it they report 0 B.
rm -f "${ROOTFS_DIR}/etc/mtab" 2>/dev/null || true
ln -sf /proc/self/mounts "${ROOTFS_DIR}/etc/mtab" 2>/dev/null || true

# gdk-pixbuf loader registry: generated below by gdk-pixbuf-query-loaders
# (without it every themed-icon load fails: xfce4-panel dies at startup
# in ensure_surface_for_gicon with Gtk:ERROR → abort, see serial log).

# Generated databases (same category: built at image time, not patches).
# pacman hooks would do this on a live system; here one native pass covers
# everything. Target binaries are Arch glibc same-arch builds, so they run
# natively under unshare+chroot - no qemu involved.
target_run() {
    unshare -rm chroot "${ROOTFS_DIR}" "$@"
}

# A Qt platform-plugin abort can leave the Plasma session waiting for a
# D-Bus helper that never registered. Validate XCB and its transitive library
# dependencies in the target namespace before assembling another disk image.
echo "[*] Checking Qt6 X11 platform plugin dependencies..."
if [ ! -f "${ROOTFS_DIR}/usr/lib/qt6/plugins/platforms/libqxcb.so" ]; then
    echo "[!] Missing Qt6 XCB plugin; reinstall qt6-base in the target rootfs." >&2
    exit 1
fi
if ! target_run /usr/lib/ld-linux-x86-64.so.2 --list \
    /usr/lib/qt6/plugins/platforms/libqxcb.so > /dev/null; then
    echo "[!] Qt6 XCB dependencies are incomplete; repair the packages before rebuilding the image." >&2
    exit 1
fi

# PackageKit uses the target's pacman configuration and keyring, not the
# host-side bootstrap configuration (which disables signature checking).
if [ -x "${ROOTFS_DIR}/usr/bin/pacman-key" ]; then
    echo "[*] Initializing target Arch package signing keys..."
    if ! target_run /usr/bin/pacman-key --init || \
       ! target_run /usr/bin/pacman-key --populate archlinux; then
        echo "[!] Target keyring initialization failed; signed package installs need pacman-key --init and --populate archlinux in the guest." >&2
    fi
fi

if [ -d "${ROOTFS_DIR}/usr/share/mime" ] && \
   [ -x "${ROOTFS_DIR}/usr/bin/update-mime-database" ]; then
    echo "[*] Updating MIME database..."
    target_run /usr/bin/update-mime-database /usr/share/mime 2>/dev/null || true
fi

mkdir -p "${ROOTFS_DIR}/var/cache/fontconfig"
if [ -x "${ROOTFS_DIR}/usr/bin/fc-cache" ]; then
    echo "[*] Building font cache..."
    target_run /usr/bin/fc-cache -f 2>/dev/null || true
else
    echo "[!] No fc-cache in target; apps will rebuild font index per launch" >&2
fi

if [ -d "${ROOTFS_DIR}/usr/share/glib-2.0/schemas" ] && \
   [ -x "${ROOTFS_DIR}/usr/bin/glib-compile-schemas" ]; then
    echo "[*] Compiling GSettings schemas..."
    target_run /usr/bin/glib-compile-schemas /usr/share/glib-2.0/schemas 2>/dev/null || true
else
    echo "[!] No glib-compile-schemas in target; GSettings apps will abort at startup" >&2
fi

if [ -d "${ROOTFS_DIR}/usr/lib/gio/modules" ] && \
   [ -x "${ROOTFS_DIR}/usr/bin/gio-querymodules" ]; then
    echo "[*] Caching GIO modules..."
    target_run /usr/bin/gio-querymodules /usr/lib/gio/modules 2>/dev/null || true
fi
if [ -x "${ROOTFS_DIR}/usr/bin/gdk-pixbuf-query-loaders" ]; then
    echo "[*] Caching gdk-pixbuf loaders..."
    target_run /usr/bin/gdk-pixbuf-query-loaders --update-cache 2>/dev/null || true
fi
if [ -x "${ROOTFS_DIR}/usr/bin/gtk-query-immodules-3.0" ]; then
    echo "[*] Caching GTK input-method modules..."
    target_run /usr/bin/gtk-query-immodules-3.0 --update-cache 2>/dev/null || true
fi

# libwnck default_icon_at_size abort fix (xfce4-session SIGABRT, base==NULL):
# libwnck scales the default window icon; when the Gtk icon lookup returns
# NULL (no icon-theme.cache, no SVG/PNG loader, or no fallback theme for
# root with HOME=/) it calls g_error() -> abort(). Rebuild the icon caches
# here (pacman hooks would do it on live Arch) and seed a fallback theme.
if [ -x "${ROOTFS_DIR}/usr/bin/gtk-update-icon-cache" ]; then
    echo "[*] Caching GTK icon themes..."
    for _theme in hicolor Adwaita; do
        if [ -d "${ROOTFS_DIR}/usr/share/icons/${_theme}" ]; then
            target_run /usr/bin/gtk-update-icon-cache -f -t "/usr/share/icons/${_theme}" 2>/dev/null || true
        fi
    done
else
    echo "[!] No gtk-update-icon-cache in target; libwnck icon lookup may abort" >&2
fi

# XDG compat: Gtk joins $XDG_DATA_DIRS entries with "/mime/..."; with the
# default "/usr/local/share:/usr/share" a missing /usr/local/share/mime
# yields lookups like /usr/local/share/mime//generic-icons (see fault log).
# Keep the dir present so the lookup is empty-but-valid, not ENOENT.
mkdir -p "${ROOTFS_DIR}/usr/local/share/mime/packages" "${ROOTFS_DIR}/usr/local/share/icons" 2>/dev/null || true
if [ -x "${ROOTFS_DIR}/usr/bin/update-mime-database" ]; then
    target_run /usr/bin/update-mime-database /usr/local/share/mime 2>/dev/null || true
fi

# XFCE reverse-DNS rename compat: Arch ships org.xfce.session.png, but
# libwnck/xinitrc-era lookups and weston.ini still request the legacy
# xfce4-session name. A missing name -> NULL base pixbuf -> g_error abort.
# Provide legacy aliases, then rebuild hicolor cache to include them.
if [ -d "${ROOTFS_DIR}/usr/share/icons/hicolor" ]; then
    for _f in "${ROOTFS_DIR}"/usr/share/icons/hicolor/*/apps/org.xfce.session.png; do
        [ -e "${_f}" ] || continue
        _d=$(dirname "${_f}")
        [ -e "${_d}/xfce4-session.png" ] || ln -sf "org.xfce.session.png" "${_d}/xfce4-session.png" 2>/dev/null || true
    done
    mkdir -p "${ROOTFS_DIR}/usr/share/pixmaps" 2>/dev/null || true
    if [ ! -e "${ROOTFS_DIR}/usr/share/pixmaps/xfce4-session.png" ]; then
        _src=$(ls "${ROOTFS_DIR}"/usr/share/icons/hicolor/48x48/apps/org.xfce.session.png \
                 "${ROOTFS_DIR}"/usr/share/icons/hicolor/32x32/apps/org.xfce.session.png 2>/dev/null | head -n 1)
        if [ -n "${_src}" ]; then
            cp -f "${_src}" "${ROOTFS_DIR}/usr/share/pixmaps/xfce4-session.png" 2>/dev/null || true
        fi
    fi
    if [ -x "${ROOTFS_DIR}/usr/bin/gtk-update-icon-cache" ]; then
        target_run /usr/bin/gtk-update-icon-cache -f -t /usr/share/icons/hicolor 2>/dev/null || true
    fi
fi

# Fallback Gtk theme for root (HOME=/) and new users: without this Gtk has
# no icon-theme-name and libwnck gets NULL as its base pixbuf.
mkdir -p "${ROOTFS_DIR}/etc/gtk-3.0" "${ROOTFS_DIR}/root/.config/gtk-3.0" "${ROOTFS_DIR}/etc/skel/.config/gtk-3.0" 2>/dev/null || true
for _gtkini in "${ROOTFS_DIR}/etc/gtk-3.0/settings.ini" "${ROOTFS_DIR}/root/.config/gtk-3.0/settings.ini" "${ROOTFS_DIR}/etc/skel/.config/gtk-3.0/settings.ini"; do
    if [ ! -f "${_gtkini}" ]; then
        cat > "${_gtkini}" <<'EOF'
[Settings]
gtk-icon-theme-name=Adwaita
gtk-fallback-icon-theme=hicolor
gtk-theme-name=Adwaita
EOF
    fi
done

# Weston/XWayland entry point referenced by initrd/weston.ini and
# initrd/startw.sh. Upstream Arch ships startxfce4, not this name, so
# provide a small wrapper with sane XDG/GTK defaults for XFCE-on-XWayland.
if [ ! -e "${ROOTFS_DIR}/usr/bin/start-xfce4-wayland" ]; then
    echo "[*] Installing start-xfce4-wayland wrapper..."
    cat > "${ROOTFS_DIR}/usr/bin/start-xfce4-wayland" <<'EOF'
#!/bin/sh
# AvoryOS: launch XFCE4 inside XWayland (Weston) with icon/mime-safe env.
export XDG_DATA_DIRS="/usr/local/share:/usr/share${XDG_DATA_DIRS:+:$XDG_DATA_DIRS}"
export XDG_CONFIG_HOME="${XDG_CONFIG_HOME:-$HOME/.config}"
export XDG_DATA_HOME="${XDG_DATA_HOME:-$HOME/.local/share}"
export NO_AT_BRIDGE=1
export GTK_A11Y=none
if [ -z "$DBUS_SESSION_BUS_ADDRESS" ] && command -v dbus-run-session >/dev/null 2>&1; then
    exec dbus-run-session -- startxfce4 "$@"
else
    exec startxfce4 "$@"
fi
EOF
    chmod 0755 "${ROOTFS_DIR}/usr/bin/start-xfce4-wayland"
fi

# Ensure X11/XFCE startup scripts are executable
chmod 0755 "${ROOTFS_DIR}/etc/X11/xinit/xinitrc" 2>/dev/null || true
chmod 0755 "${ROOTFS_DIR}/etc/X11/xinit/xserverrc" 2>/dev/null || true
chmod 0755 "${ROOTFS_DIR}/etc/xdg/xfce4/xinitrc" 2>/dev/null || true

# Ensure dbus helper has user read permission so debugfs can write it into the disk image
chmod u+r "${ROOTFS_DIR}/usr/lib/dbus-daemon-launch-helper" 2>/dev/null || true
chmod 4755 "${ROOTFS_DIR}/usr/lib/dbus-daemon-launch-helper" 2>/dev/null || true

# Wrap xfce4-session in dbus-run-session in xinitrc if present
if [ -f "${ROOTFS_DIR}/etc/xdg/xfce4/xinitrc" ]; then
    sed -i 's/exec \${XFCE4_SESSION_COMPOSITOR:-xfce4-session}/if [ -z "$DBUS_SESSION_BUS_ADDRESS" ] \&\& command -v dbus-run-session >\/dev\/null 2>\&1; then exec dbus-run-session -- \${XFCE4_SESSION_COMPOSITOR:-xfce4-session}; else exec \${XFCE4_SESSION_COMPOSITOR:-xfce4-session}; fi/' "${ROOTFS_DIR}/etc/xdg/xfce4/xinitrc" 2>/dev/null || true
fi
