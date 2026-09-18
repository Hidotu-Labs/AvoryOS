#!/usr/bin/env bash
# scripts/setup-alpine.sh - Downloads and installs Alpine Linux rootfs into AvoryOS disk image
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DISK_IMG="${ROOT_DIR}/disk.img"
BUILD_DIR="${ROOT_DIR}/build/alpine"
POPULATE_SCRIPT="${ROOT_DIR}/scripts/populate-ext2-dir.sh"

ALPINE_BRANCH="v3.21"
# QtBase/modules that contain the QSystemLocale post-destruction fix (6.8.3+).
QT6_BRANCH="v3.22"
ALPINE_VERSION="3.21.0"
ALPINE_TARBALL="alpine-minirootfs-${ALPINE_VERSION}-x86_64.tar.gz"
ALPINE_URL="https://dl-cdn.alpinelinux.org/alpine/${ALPINE_BRANCH}/releases/x86_64/${ALPINE_TARBALL}"

mkdir -p "${BUILD_DIR}"

# 1. Download Alpine rootfs if not present
if [ ! -f "${BUILD_DIR}/${ALPINE_TARBALL}" ]; then
    echo "[*] Downloading Alpine ${ALPINE_VERSION}..."
    curl -L "${ALPINE_URL}" -o "${BUILD_DIR}/${ALPINE_TARBALL}"
fi

# 2. Extract rootfs to a temporary location if not already present.
#    A rootfs built for a different release branch is discarded first: the
#    package markers below are keyed by branch, but files that only existed in
#    the old branch would otherwise survive and shadow their replacements.
ROOTFS_DIR="${BUILD_DIR}/rootfs"
ROOTFS_RELEASE=""
if [ -f "${ROOTFS_DIR}/etc/alpine-release" ]; then
    ROOTFS_RELEASE="$(cat "${ROOTFS_DIR}/etc/alpine-release")"
fi
case "${ROOTFS_RELEASE}" in
    "${ALPINE_BRANCH#v}."*)
        # Already the selected branch.
        ;;
    "")
        # No rootfs yet; the extraction below creates it.
        ;;
    *)
        echo "[*] rootfs is Alpine ${ROOTFS_RELEASE}, switching to ${ALPINE_BRANCH}; rebuilding it from scratch..."
        rm -rf "${ROOTFS_DIR}"
        ;;
esac
if [ ! -d "${ROOTFS_DIR}/etc" ]; then
    echo "[*] extracting Alpine rootfs to ${ROOTFS_DIR}..."
    mkdir -p "${ROOTFS_DIR}"
    tar -xzf "${BUILD_DIR}/${ALPINE_TARBALL}" -C "${ROOTFS_DIR}"
fi
printf '%s\n' "${ALPINE_BRANCH}" > "${ROOTFS_DIR}/etc/avoryos-alpine-branch"

# 3. Helper to download and install Alpine packages manually
install_apk() {
    local PKG_NAME=$1
    local REPO=$2
    local BRANCH=${3:-"${ALPINE_BRANCH}"}
    local PKG_MARKER="${ROOTFS_DIR}/etc/avoryos-pkg/${BRANCH}-${REPO}-${PKG_NAME}"
    
    if [ -f "${PKG_MARKER}" ]; then
        echo "[*] Package ${PKG_NAME} already installed, skipping."
        return 0
    fi

    echo "[*] Installing package: ${PKG_NAME} from ${REPO} (branch: ${BRANCH})..."
    
    # Escape dots and pluses in PKG_NAME for grep
    local ESCAPED_PKG_NAME=$(echo "${PKG_NAME}" | sed 's/\./\\./g;s/+/\\+/g')
    local APK_FILENAME=$(curl -sL "https://dl-cdn.alpinelinux.org/alpine/${BRANCH}/${REPO}/x86_64/" | grep -oP ">${ESCAPED_PKG_NAME}-[0-9][^<]*\.apk<" | sed 's/>//;s/<//' | sort -V | tail -n 1)
    
    # Fallback to a known version if the search fails
    if [ -z "${APK_FILENAME}" ] && [ "${PKG_NAME}" == "st" ]; then
        APK_FILENAME="st-0.9.2-r0.apk"
    fi
    
    if [ -z "${APK_FILENAME}" ]; then
        echo "[!] Could not find package ${PKG_NAME} in ${REPO} (${BRANCH})"
        return 1
    fi
    
    local APK_URL="https://dl-cdn.alpinelinux.org/alpine/${BRANCH}/${REPO}/x86_64/${APK_FILENAME}"
    
    if [ ! -f "${BUILD_DIR}/${APK_FILENAME}" ]; then
        echo "[*] Downloading ${APK_URL}..."
        curl -L "${APK_URL}" -o "${BUILD_DIR}/${APK_FILENAME}"
    fi
    
    # APK files are 3 concatenated gzip streams (signature + control + data).
    # tar --ignore-zeros -xz processes all streams in the concatenation.
    tar --ignore-zeros -xzf "${BUILD_DIR}/${APK_FILENAME}" -C "${ROOTFS_DIR}" --warning=no-unknown-keyword 2>/dev/null || true

    # The control stream also unpacks its metadata into the root of the rootfs
    # (/.PKGINFO, hook scripts, ...).  Drop them again: a stray /.flatpak-info
    # makes KF6's KSandbox::isFlatpak() answer true, and konsole then boots
    # each session through its sandbox code path (it shells out to "getent"
    # and blocks in QProcess::waitForFinished()), which stops it opening.
    for stray in .PKGINFO .SIGN.* .pre-install .post-install .pre-upgrade \
                 .post-upgrade .pre-deinstall .post-deinstall .trigger \
                 .flatpak-info; do
        rm -f "${ROOTFS_DIR}"/${stray}
    done

    # Mark as installed
    mkdir -p "${ROOTFS_DIR}/etc/avoryos-pkg"
    touch "${PKG_MARKER}"
}

# Install st terminal and X11 utilities and their dependencies
install_apk "st" "community"
install_apk "feh" "community"
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

# Install KCalc and Qt6/KF6 runtime dependencies from Alpine community repo
install_apk "libgomp" "main"
install_apk "double-conversion" "community"
install_apk "libb2" "community"
# QtBase v3.21 ships 6.8.2, which segfaults (null-this QReadWriteLock) whenever
# QSystemLocale::query() runs after the QSystemLocaleData global static has been
# destroyed, e.g. from a static destructor during process exit.  Upstream fixed
# it in qtbase commit d5c5f9f3529b ("QSystemLocale: bail out if accessed
# post-destruction"), first released in Qt 6.8.3.  v3.22 is the first Alpine
# branch carrying 6.8.3, so pull the QtBase packages from there; the rest of the
# rootfs stays on v3.21 (Qt keeps ABI compatibility across patch releases).
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

# IceWM window manager and dependencies
echo "[*] Installing IceWM window manager..."
install_apk "icewm" "community"
install_apk "libxinerama" "main"

# OpenAL audio library for native games/apps
echo "[*] Installing OpenAL Soft..."
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

# Openbox window manager (LXDE base) and dependencies
install_apk "libxml2" "main"
install_apk "startup-notification" "community"
install_apk "libxcomposite" "main"
install_apk "libxdamage" "main"



# X11 utilities and toolkit libraries (xclock, xterm, etc.)
echo "[*] Installing Xorg Server and DRM drivers..."
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
# libdrm-tests ships modetest, used as the Phase 4 bochs canary cross-check
# (AvoryOS kernel tests + bin/test_kpi_bochs are the primary evidence).
install_apk "libdrm-tests" "main"
install_apk "mesa-gbm" "main"
install_apk "mesa-egl" "main"
install_apk "nettle" "main"
install_apk "libmagic" "main"

echo "[*] Installing X11 utilities and toolkit libraries..."
install_apk "libxt" "main"
install_apk "libxmu" "main"
install_apk "libxaw" "main"
install_apk "libxext" "main"
install_apk "libxkbfile" "main"

# Minimal GTK (GTK 2.0) and core dependencies
echo "[*] Installing GTK 2.0 and core dependencies..."
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

# GTK 3.0 and its core dependencies
echo "[*] Installing GTK 3.0 and dependencies..."
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
# Alpine names the classic GLX demo "gears"; preserve the conventional command.
rm -f "${ROOTFS_DIR}/usr/bin/glxgears"
cp "${ROOTFS_DIR}/usr/bin/gears" "${ROOTFS_DIR}/usr/bin/glxgears"
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

# VLC Media Player and audio/video codec dependencies
echo "[*] Installing VLC Media Player..."
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
# libavcodec.so (used by VLC's avcodec plugin) links these at load time; without
# them the plugin fails to dlopen and VLC falls back to libmad for MP3.
install_apk "libvpx" "community"
install_apk "lame-libs" "main"
install_apk "libtheora" "main"
install_apk "x264-libs" "community"
install_apk "x265-libs" "community"
install_apk "xvidcore" "community"
# Runtime deps of libavformat/libavcodec (VLC avcodec plugin dlopens these).
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

# Configure D-Bus machine-id for VLC & DBus clients
mkdir -p "${ROOTFS_DIR}/var/lib/dbus" "${ROOTFS_DIR}/etc"
if [ ! -f "${ROOTFS_DIR}/etc/machine-id" ]; then
    echo "b08040a9e7114fea9634b7f94da7e722" > "${ROOTFS_DIR}/etc/machine-id"
fi
ln -sf /etc/machine-id "${ROOTFS_DIR}/var/lib/dbus/machine-id"

# Allow VLC execution as root. LD_PRELOAD UID spoofing breaks D-Bus EXTERNAL auth
# (libdbus sends fake uid 1000 while the kernel reports uid 0), so patch the one
# geteuid()==0 early-exit in vlc instead.
if [ -f "${ROOTFS_DIR}/usr/bin/vlc.bin" ]; then
    mv -f "${ROOTFS_DIR}/usr/bin/vlc.bin" "${ROOTFS_DIR}/usr/bin/vlc"
fi
rm -f "${ROOTFS_DIR}/lib/libvlc_root_fix.so"
if [ -f "${ROOTFS_DIR}/usr/bin/vlc" ]; then
    python3 - <<PY
from pathlib import Path
path = Path("${ROOTFS_DIR}/usr/bin/vlc")
data = bytearray(path.read_bytes())

WANT   = bytes.fromhex("0f842f010000")  # je +0x12f  (root-guard branch)
NOP6   = b"\x90" * 6
HINT   = 0x109c                         # known offset for current Alpine VLC

# 1. Check if already patched at the hint offset – nothing to do.
if data[HINT:HINT + 6] == NOP6:
    print("[OK] vlc root guard already patched, skipping")
    raise SystemExit(0)

# 2. Try the known offset first.
if data[HINT:HINT + 6] == WANT:
    off = HINT
else:
    # 3. Scan the binary for the je pattern (handles VLC version changes).
    off = data.find(WANT)
    if off == -1:
        raise SystemExit(
            f"vlc root guard: pattern not found and offset 0x{HINT:x} "
            f"has unexpected bytes: {data[HINT:HINT + 6].hex()}"
        )
    print(f"[~] vlc root guard found at 0x{off:x} (hint was 0x{HINT:x})")

data[off:off + 6] = NOP6
path.write_bytes(data)
print(f"[OK] patched vlc root guard at 0x{off:x}")
PY
    chmod +x "${ROOTFS_DIR}/usr/bin/vlc"
fi

# Seed VLC defaults: ALSA output, no duplicate Qt/privacy prompts, no dbus control.
mkdir -p "${ROOTFS_DIR}/etc/vlc" "${ROOTFS_DIR}/root/.config/vlc"
cat > "${ROOTFS_DIR}/etc/vlc/vlcrc" <<'VLCRC_EOF'
[core]
control=
dbus=0
mpris=0

[dbus]
control=0
mpris=0

[alsa]
alsa-audio-device=hw:0,0

[qt]
qt-privacy-ask=0

[codec]
codec=avcodec

[clock]
clock-synchro=0

[file]
file-caching=50

[network]
network-caching=50

[disc]
disc-caching=50
VLCRC_EOF
cp "${ROOTFS_DIR}/etc/vlc/vlcrc" "${ROOTFS_DIR}/root/.config/vlc/vlcrc"

# libmad fails MP3 decode on AvoryOS; libavcodec handles MP3 reliably.
rm -f "${ROOTFS_DIR}/usr/lib/vlc/plugins/audio_filter/libmad_plugin.so"

# Generate VLC plugin cache
if [ -x "${ROOTFS_DIR}/usr/lib/vlc/vlc-cache-gen" ]; then
    echo "[*] Generating VLC plugin cache..."
    if command -v qemu-x86_64 >/dev/null 2>&1 && [ -f "${ROOTFS_DIR}/lib/ld-musl-x86_64.so.1" ]; then
        qemu-x86_64 -L "${ROOTFS_DIR}" "${ROOTFS_DIR}/lib/ld-musl-x86_64.so.1" --library-path "${ROOTFS_DIR}/lib:${ROOTFS_DIR}/usr/lib" "${ROOTFS_DIR}/usr/lib/vlc/vlc-cache-gen" "${ROOTFS_DIR}/usr/lib/vlc/plugins" 2>/dev/null || true
    fi
fi

# Keep musl's runtime linker search path explicit inside AvoryOS.
# Some early userspace paths only reliably resolve shared objects from /lib.
mkdir -p "${ROOTFS_DIR}/etc" "${ROOTFS_DIR}/lib"
cat > "${ROOTFS_DIR}/etc/ld-musl-x86_64.path" <<'EOF'
/lib
/usr/local/lib
/usr/lib
EOF
safe_cp() {
    local src="$1"
    local dst="$2"
    [ -f "$src" ] || return 0
    if [ -e "$dst" ]; then
        [ "$(realpath "$src" 2>/dev/null)" = "$(realpath "$dst" 2>/dev/null)" ] && return 0
    fi
    cp -f "$src" "$dst" 2>/dev/null || true
}

safe_cp "${ROOTFS_DIR}/usr/lib/libucontext.so.1" "${ROOTFS_DIR}/lib/libucontext.so.1"
safe_cp "${ROOTFS_DIR}/usr/lib/libucontext_posix.so.1" "${ROOTFS_DIR}/lib/libucontext_posix.so.1"

# glibc-linked tools such as /opt/coreutils/bin/ls search lib64 paths.
mkdir -p "${ROOTFS_DIR}/lib64" "${ROOTFS_DIR}/usr/lib64"
safe_cp "${ROOTFS_DIR}/lib/libc.musl-x86_64.so.1" "${ROOTFS_DIR}/lib64/libc.musl-x86_64.so.1"
safe_cp "${ROOTFS_DIR}/lib/libc.musl-x86_64.so.1" "${ROOTFS_DIR}/usr/lib64/libc.musl-x86_64.so.1"
safe_cp "${ROOTFS_DIR}/usr/lib/libucontext.so.1" "${ROOTFS_DIR}/lib64/libucontext.so.1"
safe_cp "${ROOTFS_DIR}/usr/lib/libucontext.so.1" "${ROOTFS_DIR}/usr/lib64/libucontext.so.1"
safe_cp "${ROOTFS_DIR}/usr/lib/libucontext_posix.so.1" "${ROOTFS_DIR}/lib64/libucontext_posix.so.1"
safe_cp "${ROOTFS_DIR}/usr/lib/libucontext_posix.so.1" "${ROOTFS_DIR}/usr/lib64/libucontext_posix.so.1"

install_apk "mesa-glapi" "main"
install_apk "llvm19-libs" "main"
install_apk "libelf" "main"
install_apk "zstd-libs" "main"
install_apk "musl-obstack" "main"

# glibc-linked coreutils also need musl-obstack in lib64 paths.
mkdir -p "${ROOTFS_DIR}/lib64" "${ROOTFS_DIR}/usr/lib64"
safe_cp "${ROOTFS_DIR}/usr/lib/libobstack.so.1" "${ROOTFS_DIR}/lib64/libobstack.so.1"
safe_cp "${ROOTFS_DIR}/usr/lib/libobstack.so.1" "${ROOTFS_DIR}/usr/lib64/libobstack.so.1"
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

# cmus audio dependencies
echo "[*] Installing cmus audio codecs and libraries..."
install_apk "libflac" "main"
install_apk "alsa-lib" "main"
install_apk "faad2-libs" "community"
install_apk "libmad" "community"
install_apk "libvorbis" "main"
install_apk "wavpack-libs" "community"
install_apk "opusfile" "main"
# ffmpeg libs (for additional codec support)
install_apk "ffmpeg-libavcodec" "community"
install_apk "ffmpeg-libavformat" "community"
# libasyncns is needed by libpulsecommon (inside libpulse)
install_apk "libasyncns" "community"
install_apk "libpulse" "community"
# libpulse-mainloop-glib carries libpulse-mainloop-glib.so.0 (needed by pulseaudio-qt)
install_apk "libpulse-mainloop-glib" "community"

install_apk "xkeyboard-config" "main"
# setxkbmap binary: the Plasma keyboard KCM uses it to apply layouts
install_apk "setxkbmap" "community"
install_apk "font-dejavu" "main"
install_apk "sl" "community"
install_apk "gifsicle" "community"

# GTK2 Development headers (for host compilation)
echo "[*] Installing GTK 2.0 development packages..."
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

# Compiler / toolchain tools for AUR package compilation
echo "[*] Installing compilation tools..."
install_apk "make" "main"
install_apk "gcc" "main"
# gcc runtime shared-library dependencies (cc1/lto1 link against these)
install_apk "isl26" "main"
install_apk "mpfr4" "main"
install_apk "mpc1" "main"
install_apk "musl-dev" "main"
install_apk "binutils" "main"



# GTK 3.0 Development headers
echo "[*] Installing GTK 3.0 development packages..."
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

# ── XFCE4 Desktop Environment ────────────────────────────────────────────
# XFCE4 runs on top of XWayland (Weston provides the Wayland compositor;
# XFCE4 components run as X11 clients on the embedded XWayland display).
echo "[*] Installing XFCE4 desktop environment..."
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
# xinit 1.4.2 depends on xauth, mcookie, xmodmap and xrdb.  Packages are
# unpacked here without apk dependency resolution, so pull them in explicitly;
# without mcookie/xauth, startx(1) aborts with "Couldn't create cookie".
install_apk "mcookie" "main"
install_apk "xauth" "community"
install_apk "xmodmap" "community"
ln -sf elogind/libelogind-shared-252.so "${ROOTFS_DIR}/usr/lib/libelogind-shared-252.so" 2>/dev/null || true

# Patch lightdm-gtk-greeter embedded UI signal for GreeterMenuBar (bypasses missing signal in standalone GtkBuilder load)
if [ -f "${ROOTFS_DIR}/usr/bin/lightdm-gtk-greeter" ]; then
    python3 - <<PY
from pathlib import Path
path = Path("${ROOTFS_DIR}/usr/bin/lightdm-gtk-greeter")
data = bytearray(path.read_bytes())
target = b'<signal name="key-press-event" handler="menubar_key_press_cb" swapped="no"/>'
off = data.find(target)
if off != -1:
    data[off:off + len(target)] = b" " * len(target)
    path.write_bytes(data)
    print(f"[OK] patched lightdm-gtk-greeter UI signal at 0x{off:x}")
else:
    print("[OK] lightdm-gtk-greeter already patched or target not found")
PY
    chmod +x "${ROOTFS_DIR}/usr/bin/lightdm-gtk-greeter"
fi

# Configure LightDM and GTK Greeter
mkdir -p "${ROOTFS_DIR}/etc/lightdm"
cat > "${ROOTFS_DIR}/etc/lightdm/lightdm.conf" <<'EOF'
[LightDM]
run-directory=/run/lightdm
start-default-seat=true
logind-load-seats=false
logind-check-graphical=false

[Seat:*]
type=local
greeter-session=lightdm-gtk-greeter
greeter-hide-users=false
user-session=xfce
xserver-command=/usr/libexec/Xorg -noreset -nolisten tcp -ac
session-wrapper=/etc/X11/xinit/Xsession
EOF

cat > "${ROOTFS_DIR}/etc/lightdm/lightdm-gtk-greeter.conf" <<'EOF'
[greeter]
at-spi-enabled=false
indicators=~host;~spacer;~clock;~power
theme-name=Adwaita
icon-theme-name=Adwaita
EOF

mkdir -p "${ROOTFS_DIR}/etc/pam.d"
cat > "${ROOTFS_DIR}/etc/pam.d/lightdm-greeter" <<'EOF'
#%PAM-1.0
auth      required  pam_permit.so
account   required  pam_permit.so
password  required  pam_deny.so
session   required  pam_env.so
session   required  pam_unix.so
-session  optional  pam_limits.so
EOF

cat > "${ROOTFS_DIR}/etc/pam.d/lightdm" <<'EOF'
#%PAM-1.0
auth      required  pam_permit.so
account   required  pam_permit.so
password  required  pam_deny.so
session   required  pam_env.so
session   required  pam_unix.so
-session  optional  pam_limits.so
EOF

cat > "${ROOTFS_DIR}/etc/pam.d/lightdm-autologin" <<'EOF'
#%PAM-1.0
auth      required  pam_permit.so
account   required  pam_permit.so
password  required  pam_deny.so
session   required  pam_env.so
session   required  pam_unix.so
-session  optional  pam_limits.so
EOF

# Plugins commonly expected to exist at XFCE4 startup
install_apk "xfce4-panel-dev" "community"
install_apk "xfce4-battery-plugin" "community"
install_apk "xfce4-clipman-plugin" "community" "edge"
install_apk "xfce4-systemload-plugin" "community" "edge"
install_apk "xfce4-whiskermenu-plugin" "community"

# Additional XFCE4 dependencies
install_apk "libnotify" "community"
install_apk "notification-daemon" "testing" "edge"
install_apk "polkit" "community"
install_apk "polkit-elogind" "community"
install_apk "upower" "community"
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

# xfce4-session hard dependency — libwnck3
install_apk "libwnck3" "community"
# xrdb is called by startxfce4 / .xinitrc to load X resources
install_apk "xrdb" "community"
# xhost needed by some XFCE4 components when switching displays
install_apk "xhost" "community"

# Mousepad (Text Editor) + GTK Plumbing
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

# PCManFM (Lightweight File Manager)
echo "[*] Installing PCManFM file manager and dependencies..."
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

# SDL2 and related libraries
echo "[*] Installing SDL2 and related libraries..."
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

# SDL3 and related libraries (required by Mocktail and modern applications)
echo "[*] Installing SDL3 and related libraries..."
install_apk "sdl3" "community" "edge"
install_apk "sdl3-dev" "community" "edge"
install_apk "sdl3_ttf" "community" "edge"
install_apk "sdl3_ttf-dev" "community" "edge"
# sdl3_image and sdl3_mixer are not yet packaged in Alpine edge; skip for now
# Mocktail rendering pipeline (OpenGL/EGL backend — no Vulkan required)
install_apk "libplacebo" "community" "edge"
install_apk "libplacebo-dev" "community" "edge"
# Capstone disassembly engine v5 (required by Mocktail's JIT/ABI layer)
install_apk "capstone" "community" "edge"
install_apk "capstone-dev" "community" "edge"

# WebKitGTK (GTK 3 / libsoup 3 ABI)
#
# Packages are extracted manually by install_apk(), so apk cannot resolve the
# shared-library providers for us. Keep WebKitGTK on the same release branch as
# GTK and install its non-core runtime providers explicitly before the engine.
echo "[*] Installing WebKitGTK and dependencies..."
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
# gst-plugins-bad is unpacked without apk dependency resolution. Install the
# codec/scanner libraries used by its voaacenc, voamrwbenc, webrtc and zbar
# plugins explicitly so gst-plugin-scanner can load them.
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

# GStreamer plugin libraries are dlopen()ed by WebKit's media engine, so apk
# dependency metadata cannot see their providers.  Install every runtime
# provider explicitly; otherwise the plugins are present but fail to load and
# format support silently degrades.
#
# gst-libav supplies the software decoders (avdec_h264, avdec_aac, ...).
# Alpine's gst-plugins-good/bad/ugly ship no faad plugin, so this is the only
# software H.264/AAC path besides openh264/v4l2codecs (hardware).
install_apk "gst-libav" "community"
# gst-libav links libavcodec/libavfilter/libavformat/libavutil.  Only
# ffmpeg-libavcodec/-libavformat are installed earlier, and they cannot even
# load without libavutil.so.58, so complete the FFmpeg 6 runtime here.
install_apk "ffmpeg-libavutil" "community"
install_apk "ffmpeg-libavfilter" "community"
install_apk "ffmpeg-libswscale" "community"
install_apk "ffmpeg-libswresample" "community"
install_apk "ffmpeg-libpostproc" "community"
# Optional providers linked by libavcodec/libavformat (AV1, VPL, modules,
# Blu-ray, RIST, ZeroMQ) and their own dependencies.
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
# libavfilter also links libplacebo.  The edge 7.x build installed for
# Mocktail exports a different soname (libplacebo.so.360), so install the
# v3.21 build (libplacebo.so.338) that libavfilter.so.9 was built against;
# both versioned libraries coexist.  Its shader/Vulkan backend providers:
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
# Broken GStreamer plugins with present plugin files but missing providers:
# libgstde265 (HEVC), libgstopenh264 (H.264), libgstassrender (subtitles),
# libgsta52dec (AC-3), libgstopenjpeg (JPEG 2000).
install_apk "libde265" "main"
install_apk "openh264" "community"
install_apk "libass" "community"
install_apk "libunibreak" "community"
install_apk "a52dec" "community"
install_apk "openjpeg" "main"
# Remaining codec providers for shipped gst-plugins-bad/ugly plugins
# (AMR, GSM, AAC encode, tracker modules, tempo/pitch, tags, Bluetooth).
install_apk "opencore-amr" "community"
install_apk "gsm" "main"
install_apk "faac" "community"
install_apk "libmodplug" "community"
install_apk "soundtouch" "community"
install_apk "libtag" "community"
install_apk "sbc" "community"
install_apk "libldac" "community"
install_apk "libfreeaptx" "community"
# ALSA resample plugins, PipeWire WebRTC echo cancellation (spa aec), and
# libcamera capture (spa libcamera).
install_apk "libsamplerate" "main"
install_apk "webrtc-audio-processing-1" "community"
install_apk "libcamera" "community"
install_apk "libcamera-ipa" "community"
# Enchant loads its backends from /usr/lib/enchant-2 at runtime; without them
# WebKit spell checking finds no dictionaries at all.
install_apk "enchant2" "community"
install_apk "enchant2-aspell" "community"
install_apk "enchant2-hunspell" "community"
install_apk "enchant2-nuspell" "community"
install_apk "enchant2-data" "community"
install_apk "aspell-libs" "main"
install_apk "libhunspell" "main"
install_apk "nuspell-libs" "community"
# Providers for the remaining low-use plugins: FireWire cameras (dc1394),
# DirectFB (plus its tslib input backend), neon HTTP, OpenEXR images.
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
# libsoup3 uses GIO's dynamically loaded TLS implementation. Since packages
# are unpacked without apk dependency resolution, install the complete
# glib-networking/GnuTLS chain explicitly; otherwise HTTPS fails with
# "TLS support is not available" even though plain HTTP works.
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
# Badwolf/WebKitGTK crash fix: Discord login drove musl's mbrtowc/locale
# code into __builtin_trap (HLT) when fonts/locales/tzdata were missing.
install_apk "font-noto" "community"
install_apk "font-noto-emoji" "community"
install_apk "musl-locales" "main"
install_apk "tzdata" "main"
# Point the system at a real zone.  Without /etc/localtime and /etc/timezone,
# Qt's QTimeZone::systemTimeZone() comes back invalid: the Plasma digital clock
# renders blank and its calendar popup shows day 0.  The CMOS RTC is read as
# UTC by the kernel, so default to UTC (change the link to your zone if needed).
ln -sf /usr/share/zoneinfo/UTC "${ROOTFS_DIR}/etc/localtime"
printf '%s\n' UTC > "${ROOTFS_DIR}/etc/timezone"
install_apk "alacritty" "community"



# NetSurf Web Browser
echo "[*] Installing NetSurf and dependencies..."
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

# 4. Finalize GTK environment
echo "[*] Setting up global audio environment variables..."
mkdir -p "${ROOTFS_DIR}/etc/profile.d" "${ROOTFS_DIR}/etc/pulse"
cat > "${ROOTFS_DIR}/etc/profile.d/gtk_avoryos.sh" << 'ENV_EOF'
export PULSE_SERVER=""
ENV_EOF
chmod +x "${ROOTFS_DIR}/etc/profile.d/gtk_avoryos.sh"

cat > "${ROOTFS_DIR}/etc/environment" << 'ENV_EOF'
PULSE_SERVER=""
ENV_EOF

cat > "${ROOTFS_DIR}/etc/pulse/client.conf" << 'PULSE_EOF'
autospawn = no
disable-shm = yes
PULSE_EOF

# Disable D-Bus activation for GVfs volume monitors to prevent 25s timeouts and abort crashes
rm -f "${ROOTFS_DIR}"/usr/share/dbus-1/services/org.gtk.vfs.*VolumeMonitor.service 2>/dev/null || true
rm -f "${ROOTFS_DIR}"/usr/lib/gio/modules/libgvfsdbus.so \
      "${ROOTFS_DIR}"/usr/lib/gio/modules/libgioremote-volume-monitor.so \
      "${ROOTFS_DIR}"/usr/lib/gio/modules/libgiognomeproxy.so 2>/dev/null || true

echo "[*] Compiling GSettings schemas..."
if [ -d "${ROOTFS_DIR}/usr/share/glib-2.0/schemas" ]; then
    if command -v glib-compile-schemas >/dev/null 2>&1; then
        glib-compile-schemas "${ROOTFS_DIR}/usr/share/glib-2.0/schemas"
    fi
fi
mkdir -p "${ROOTFS_DIR}/etc/glib-2.0/settings" \
         "${ROOTFS_DIR}/root/.config/glib-2.0/settings" \
         "${ROOTFS_DIR}/.config/glib-2.0/settings"
touch "${ROOTFS_DIR}/etc/glib-2.0/settings/defaults"

echo "[*] Updating MIME database..."
if [ -d "${ROOTFS_DIR}/usr/share/mime" ]; then
    if command -v update-mime-database >/dev/null 2>&1; then
        update-mime-database "${ROOTFS_DIR}/usr/share/mime"
    fi
fi

echo "[*] Injecting gdk-pixbuf loaders cache..."
# Manually register PNG, JPEG and SVG loaders since we can't run the query tool
LOADERS_DIR="/usr/lib/gdk-pixbuf-2.0/2.10.0"
mkdir -p "${ROOTFS_DIR}${LOADERS_DIR}"
cat > "${ROOTFS_DIR}${LOADERS_DIR}/loaders.cache" <<EOF
# GdkPixbuf Image Loader Modules file
# Automatically generated file, do not edit

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

"${LOADERS_DIR}/loaders/libpixbufloader-gif.so"
"gif" 4 "gdk-pixbuf" "GIF" "LGPL"
"image/gif" ""
"gif" ""
"GIF8" "" 100

"${LOADERS_DIR}/loaders/libpixbufloader-bmp.so"
"bmp" 5 "gdk-pixbuf" "BMP" "LGPL"
"image/bmp" "image/x-bmp" "image/x-MS-bmp" ""
"bmp" ""
"BM" "" 100

"${LOADERS_DIR}/loaders/libpixbufloader_svg.so"
"svg" 6 "gdk-pixbuf" "Scalable Vector Graphics" "LGPL"
"image/svg+xml" "image/svg" "image/svg-xml" "image/vnd.adobe.svg+xml" "text/xml-svg" "image/svg+xml-compressed" ""
"svg" "svgz" "svg.gz" ""
" <svg" "* " 100
" <!DOCTYPE svg" "* " 100

EOF

# 4a. Configure Xorg for DRM/Modesetting
echo "[*] Configuring Xorg DRM/modesetting..."
XORG_CONF_DIR="${ROOTFS_DIR}/etc/X11/xorg.conf.d"
mkdir -p "${XORG_CONF_DIR}"
rm -f "${ROOTFS_DIR}/usr/share/X11/xorg.conf.d/40-libinput.conf"
# This is the static fallback: card0 (the native virtio-vga/ascentdrm
# desktop).  startx.sh rewrites the file at session start with
# Option "kmsdev" when bin/drm-pick.sh selects another card (amdgpu), so a
# session can run on the passed GPU without touching this packaged default.
cat > "${XORG_CONF_DIR}/10-modesetting.conf" <<EOF
# Rewritten by /bin/startx.sh when bin/drm-pick.sh selects an amdgpu card
# (Phase 6 C7).  The content below is the native card0 fallback.
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

# 4a-bis. Phase 6 C7 DRM selection helpers.  drm-pick.sh prints the card a
# session should use (amdgpu when it can drive a display, else card0);
# startx.sh/startw.sh call it, and /etc/profile.d/avory-drm.sh exports
# KWIN_DRM_DEVICES for a Plasma Wayland session started from a login shell.
if [ -f "${ROOT_DIR}/initrd/drm-pick.sh" ]; then
    install -m 0755 "${ROOT_DIR}/initrd/drm-pick.sh" "${ROOTFS_DIR}/bin/drm-pick.sh"
fi
if [ -f "${ROOT_DIR}/initrd/avory-drm.sh" ]; then
    mkdir -p "${ROOTFS_DIR}/etc/profile.d"
    install -m 0644 "${ROOT_DIR}/initrd/avory-drm.sh" "${ROOTFS_DIR}/etc/profile.d/avory-drm.sh"
fi

# 4b. Configure the default X11 session for startx(1): KDE Plasma 6.
# Alpine's startx runs $HOME/.xinitrc; root's home is / on AvoryOS.  Plasma
# needs a session D-Bus and a private XDG_RUNTIME_DIR, neither of which a
# bare startx provides, so set both up here before handing off.
echo "[*] Configuring KDE Plasma 6 as default X11 session..."

mkdir -p "${ROOTFS_DIR}/etc/skel"
cat > "${ROOTFS_DIR}/etc/skel/.xinitrc" << 'EOF'
#!/bin/sh
# AvoryOS default X11 session (startx): KDE Plasma 6 on Xorg.
export XDG_SESSION_TYPE=x11
export XDG_CURRENT_DESKTOP=KDE
export XDG_SESSION_DESKTOP=KDE
export DESKTOP_SESSION=plasma
export QT_QPA_PLATFORM=xcb
export KDE_FULL_SESSION=true

# AvoryOS does not set up /run/user/<uid>; use a private dir under /tmp.
if [ -z "$XDG_RUNTIME_DIR" ]; then
    XDG_RUNTIME_DIR="/tmp/runtime-$(id -u)"
    export XDG_RUNTIME_DIR
fi
mkdir -p "$XDG_RUNTIME_DIR" && chmod 0700 "$XDG_RUNTIME_DIR"

# startx registers a second xauth entry for "<hostname>:0" (a TCP/hostname
# entry) next to the local ":0" entry.  AvoryOS sessions only use the local
# display, so drop the hostname entry.  xauth stores/prints the local entry as
# "<hostname>/unix:0" -- that one is the :0 entry and is kept.
if [ -n "$XAUTHORITY" ] && command -v xauth >/dev/null 2>&1; then
    xauth remove "$(uname -n):0" 2>/dev/null || true
fi

# startx starts neither bus.  The system bus must exist or every solid/UPower/
# UDisks/ModemManager backend fails with "Not connected to D-Bus server"
# before Plasma finishes loading; solid then retries it in a loop.
if [ ! -s /etc/machine-id ]; then
    if command -v dbus-uuidgen >/dev/null 2>&1; then
        dbus-uuidgen --ensure=/etc/machine-id 2>/dev/null || true
    fi
fi
mkdir -p /run/dbus /var/run
[ -e /var/run/dbus ] || ln -sf /run/dbus /var/run/dbus 2>/dev/null || true
if [ -z "$DBUS_SYSTEM_BUS_ADDRESS" ]; then
    DBUS_SYSTEM_BUS_ADDRESS=unix:path=/run/dbus/system_bus_socket
    export DBUS_SYSTEM_BUS_ADDRESS
fi
if [ ! -S /run/dbus/system_bus_socket ] && command -v dbus-daemon >/dev/null 2>&1; then
    dbus-daemon --system --fork 2>/dev/null || true
fi

# startplasma-x11 expects a session bus; startx does not start one.
if [ -n "$DBUS_SESSION_BUS_ADDRESS" ]; then
    exec startplasma-x11
else
    exec dbus-run-session -- startplasma-x11
fi
EOF
chmod +x "${ROOTFS_DIR}/etc/skel/.xinitrc"

# Root's home is / on AvoryOS.
cp "${ROOTFS_DIR}/etc/skel/.xinitrc" "${ROOTFS_DIR}/.xinitrc"

# Minimal Openbox rc.xml (no dbus dependency, clean keybinds)
mkdir -p "${ROOTFS_DIR}/etc/xdg/openbox"
cat > "${ROOTFS_DIR}/etc/xdg/openbox/rc.xml" << 'EOF'
<?xml version="1.0" encoding="UTF-8"?>
<openbox_config xmlns="http://openbox.org/3.4/rc"
                xmlns:xi="http://www.w3.org/2001/XInclude">
  <resistance><strength>10</strength><screen_edge_strength>20</screen_edge_strength></resistance>
  <focus><focusNew>yes</focusNew><followMouse>no</followMouse><focusLast>yes</focusLast></focus>
  <placement><policy>Smart</policy></placement>
  <theme>
    <name>Clearlooks</name>
    <titleLayout>NLC</titleLayout>
    <keepBorder>yes</keepBorder>
  </theme>
  <desktops><number>2</number><firstdesk>1</firstdesk><names><name>Main</name><name>Extra</name></names></desktops>
  <resize><drawContents>yes</drawContents></resize>
  <mouse>
    <dragThreshold>8</dragThreshold>
    <doubleClickTime>200</doubleClickTime>
    <context name="Frame">
      <mousebind button="A-Left" action="Press"><action name="Focus"/><action name="Raise"/></mousebind>
      <mousebind button="A-Left" action="Drag"><action name="Move"/></mousebind>
      <mousebind button="A-Right" action="Drag"><action name="Resize"/></mousebind>
    </context>
    <context name="Titlebar">
      <mousebind button="Left" action="Drag"><action name="Move"/></mousebind>
      <mousebind button="Left" action="DoubleClick"><action name="ToggleMaximizeFull"/></mousebind>
    </context>
    <context name="Desktop">
      <mousebind button="Right" action="Press"><action name="ShowMenu"><menu>root-menu</menu></action></mousebind>
    </context>
  </mouse>
  <keyboard>
    <keybind key="A-F4"><action name="Close"/></keybind>
    <keybind key="A-Tab"><action name="NextWindow"/></keybind>
    <keybind key="A-space"><action name="ShowMenu"><menu>client-menu</menu></action></keybind>
    <keybind key="Super_L"><action name="ShowMenu"><menu>root-menu</menu></action></keybind>
  </keyboard>
  <applications/>
</openbox_config>
EOF

# Minimal right-click desktop menu
cat > "${ROOTFS_DIR}/etc/xdg/openbox/menu.xml" << 'EOF'
<?xml version="1.0" encoding="UTF-8"?>
<openbox_menu xmlns="http://openbox.org/3.4/menu">
  <menu id="root-menu" label="AvoryOS">
    <item label="Terminal (st)">
      <action name="Execute"><execute>st</execute></action>
    </item>
    <item label="Forkit Browser">
      <action name="Execute"><execute>forkit</execute></action>
    </item>
    <item label="NetSurf Browser">
      <action name="Execute"><execute>netsurf-gtk</execute></action>
    </item>
    <item label="File Manager">
      <action name="Execute"><execute>pcmanfm</execute></action>
    </item>
    <item label="Text Editor">
      <action name="Execute"><execute>mousepad</execute></action>
    </item>
    <item label="Music Player (cmus)">
      <action name="Execute"><execute>st -e cmus</execute></action>
    </item>
    <separator/>
    <item label="Reconfigure Openbox">
      <action name="Reconfigure"/>
    </item>
    <item label="Exit">
      <action name="Exit"/>
    </item>
  </menu>
</openbox_menu>
EOF



# 4c-xfce. Configure XFCE4 session to launch inside XWayland
# Weston starts XWayland automatically (xwayland=true in weston.ini).
# We provide a startxfce4 wrapper that points at the XWayland display
# Weston exports (typically :10), and a D-Bus session so XFCE4 can talk
# to its own daemons.
echo "[*] Configuring XFCE4 session for XWayland..."
mkdir -p "${ROOTFS_DIR}/usr/bin"
cat > "${ROOTFS_DIR}/usr/bin/start-xfce4-wayland" << 'XFCE_EOF'
#!/bin/sh
# start-xfce4-wayland — launch XFCE4 on the XWayland display that Weston
# exports.  Run this from a weston-terminal or from the Weston launcher.
#
# Weston exports XWayland as DISPLAY=:10 by default; try :10 first,
# then scan :0..:9 as fallback.
find_xwayland_display() {
    for d in 10 0 1 2 3 4 5; do
        if [ -S "/tmp/.X11-unix/X${d}" ]; then
            echo ":${d}"
            return 0
        fi
    done
    echo ":10"   # best guess even if socket not yet visible
}

export DISPLAY="${DISPLAY:-$(find_xwayland_display)}"
export XDG_SESSION_TYPE=x11
export XDG_CURRENT_DESKTOP=XFCE
export XCURSOR_THEME=Adwaita
export XCURSOR_SIZE=24

# Ensure XDG dirs exist
export XDG_CONFIG_HOME="${XDG_CONFIG_HOME:-${HOME}/.config}"
export XDG_DATA_HOME="${XDG_DATA_HOME:-${HOME}/.local/share}"
export XDG_CACHE_HOME="${XDG_CACHE_HOME:-${HOME}/.cache}"
mkdir -p "$XDG_CONFIG_HOME" "$XDG_DATA_HOME" "$XDG_CACHE_HOME" \
         "$HOME/Desktop" "$HOME/Templates" "$HOME/Downloads" "$HOME/Documents" \
         "$HOME/Pictures" "$HOME/Music" "$HOME/Videos" \
         "$XDG_CONFIG_HOME/gtk-3.0"

cat > "$XDG_CONFIG_HOME/user-dirs.dirs" << 'USER_DIRS_EOF'
XDG_DESKTOP_DIR="$HOME/Desktop"
XDG_DOWNLOAD_DIR="$HOME/Downloads"
XDG_TEMPLATES_DIR="$HOME/Templates"
XDG_PUBLICSHARE_DIR="$HOME/Public"
XDG_DOCUMENTS_DIR="$HOME/Documents"
XDG_MUSIC_DIR="$HOME/Music"
XDG_PICTURES_DIR="$HOME/Pictures"
XDG_VIDEOS_DIR="$HOME/Videos"
USER_DIRS_EOF

rm -rf "$XDG_CACHE_HOME"/*-socket* "$XDG_CACHE_HOME"/pcmanfm* "$XDG_CACHE_HOME"/Thunar* /tmp/.*-lock /tmp/*-socket*
if [ -f /etc/xdg/gtk-3.0/gtk.css ]; then
    cp -f /etc/xdg/gtk-3.0/gtk.css "$XDG_CONFIG_HOME/gtk-3.0/gtk.css"
fi

# Bootstrap D-Bus session if not already present
if [ -z "$DBUS_SESSION_BUS_ADDRESS" ]; then
    eval "$(dbus-launch --sh-syntax --exit-with-session)" 2>/dev/null || true
fi

exec startxfce4
XFCE_EOF
chmod +x "${ROOTFS_DIR}/usr/bin/start-xfce4-wayland"

# Drop a minimal xfce4-session.xml so first-run doesn't open the wizard
XFCE_SESSION_DIR="${ROOTFS_DIR}/etc/xdg/xfce4/xfconf/xfce-perchannel-xml"
mkdir -p "${XFCE_SESSION_DIR}"
cat > "${XFCE_SESSION_DIR}/xfce4-session.xml" << 'EOF'
<?xml version="1.0" encoding="UTF-8"?>
<channel name="xfce4-session" version="1.0">
  <property name="general" type="empty">
    <property name="FailsafeSessionName" type="string" value="Failsafe"/>
    <property name="SaveOnExit" type="bool" value="false"/>
    <property name="SessionName" type="string" value="Default"/>
    <property name="PromptOnLogout" type="bool" value="true"/>
    <property name="AutoSave" type="bool" value="false"/>
  </property>
  <property name="startup" type="empty">
    <property name="screensaver-delay" type="uint" value="0"/>
  </property>
  <property name="splash-screen" type="empty">
    <property name="engine" type="string" value=""/>
  </property>
  <property name="sessions" type="empty">
    <property name="Failsafe" type="empty">
      <property name="IsFailsafe" type="bool" value="true"/>
      <property name="Count" type="int" value="4"/>
      <property name="Client0_Command" type="array">
        <value type="string" value="xfwm4"/>
      </property>
      <property name="Client0_Priority" type="int" value="15"/>
      <property name="Client1_Command" type="array">
        <value type="string" value="xfsettingsd"/>
      </property>
      <property name="Client1_Priority" type="int" value="20"/>
      <property name="Client2_Command" type="array">
        <value type="string" value="xfce4-panel"/>
      </property>
      <property name="Client2_Priority" type="int" value="25"/>
      <property name="Client3_Command" type="array">
        <value type="string" value="xfdesktop"/>
      </property>
      <property name="Client3_Priority" type="int" value="30"/>
    </property>
  </property>
</channel>
EOF

# xfwm4 — minimal dark window decorations
cat > "${XFCE_SESSION_DIR}/xfwm4.xml" << 'EOF'
<?xml version="1.0" encoding="UTF-8"?>
<channel name="xfwm4" version="1.0">
  <property name="general" type="empty">
    <property name="theme" type="string" value="Default-dark"/>
    <property name="use_compositing" type="bool" value="true"/>
    <property name="unredirect_overlays" type="bool" value="false"/>
    <property name="box_move" type="bool" value="false"/>
    <property name="box_resize" type="bool" value="false"/>
    <property name="vblank_mode" type="string" value="off"/>
    <property name="sync_to_vblank" type="bool" value="false"/>
  </property>
</channel>
EOF

# xfce4-panel — panel 1 (top bar) and panel 2 (bottom dock)
cat > "${XFCE_SESSION_DIR}/xfce4-panel.xml" << 'EOF'
<?xml version="1.0" encoding="UTF-8"?>
<channel name="xfce4-panel" version="1.0">
  <property name="configver" type="int" value="2"/>
  <property name="panels" type="array">
    <value type="int" value="1"/>
    <value type="int" value="2"/>
    <property name="dark-mode" type="bool" value="true"/>
    <property name="panel-1" type="empty">
      <property name="position" type="string" value="p=6;x=0;y=0"/>
      <property name="length" type="uint" value="100"/>
      <property name="position-locked" type="bool" value="true"/>
      <property name="size" type="uint" value="26"/>
      <property name="plugin-ids" type="array">
        <value type="int" value="1"/>
        <value type="int" value="2"/>
        <value type="int" value="3"/>
        <value type="int" value="4"/>
        <value type="int" value="5"/>
      </property>
    </property>
    <property name="panel-2" type="empty">
      <property name="autohide-behavior" type="uint" value="0"/>
      <property name="position" type="string" value="p=10;x=0;y=0"/>
      <property name="length" type="uint" value="1"/>
      <property name="position-locked" type="bool" value="true"/>
      <property name="size" type="uint" value="48"/>
      <property name="plugin-ids" type="array">
        <value type="int" value="15"/>
        <value type="int" value="16"/>
        <value type="int" value="17"/>
        <value type="int" value="18"/>
        <value type="int" value="19"/>
        <value type="int" value="20"/>
        <value type="int" value="21"/>
      </property>
    </property>
  </property>
  <property name="plugins" type="empty">
    <property name="plugin-1" type="string" value="applicationsmenu"/>
    <property name="plugin-2" type="string" value="tasklist">
      <property name="flat-buttons" type="bool" value="true"/>
      <property name="show-labels" type="bool" value="true"/>
    </property>
    <property name="plugin-3" type="string" value="separator">
      <property name="expand" type="bool" value="true"/>
      <property name="style" type="uint" value="0"/>
    </property>
    <property name="plugin-4" type="string" value="systray"/>
    <property name="plugin-5" type="string" value="clock">
      <property name="digital-format" type="string" value="%H:%M"/>
    </property>
    <property name="plugin-15" type="string" value="showdesktop"/>
    <property name="plugin-16" type="string" value="separator"/>
    <property name="plugin-17" type="string" value="launcher">
      <property name="items" type="array">
        <value type="string" value="xfce4-terminal.desktop"/>
      </property>
    </property>
    <property name="plugin-18" type="string" value="launcher">
      <property name="items" type="array">
        <value type="string" value="thunar.desktop"/>
      </property>
    </property>
    <property name="plugin-19" type="string" value="launcher">
      <property name="items" type="array">
        <value type="string" value="netsurf.desktop"/>
      </property>
    </property>
    <property name="plugin-20" type="string" value="launcher">
      <property name="items" type="array">
        <value type="string" value="org.xfce.mousepad.desktop"/>
      </property>
    </property>
    <property name="plugin-21" type="string" value="launcher">
      <property name="items" type="array">
        <value type="string" value="xfce4-appfinder.desktop"/>
      </property>
    </property>
  </property>
</channel>
EOF

# xfce4-desktop — enable desktop icons (style 2: file/launcher icons + system icons)
cat > "${XFCE_SESSION_DIR}/xfce4-desktop.xml" << 'EOF'
<?xml version="1.0" encoding="UTF-8"?>
<channel name="xfce4-desktop" version="1.0">
  <property name="desktop-icons" type="empty">
    <property name="style" type="int" value="2"/>
    <property name="file-icons" type="empty">
      <property name="show-home" type="bool" value="true"/>
      <property name="show-filesystem" type="bool" value="true"/>
      <property name="show-trash" type="bool" value="true"/>
      <property name="show-removable" type="bool" value="true"/>
    </property>
    <property name="icon-size" type="uint" value="48"/>
  </property>
  <property name="backdrop" type="empty">
    <property name="screen0" type="empty">
      <property name="monitor0" type="empty">
        <property name="workspace0" type="empty">
          <property name="color-style" type="int" value="0"/>
          <property name="rgba1" type="array">
            <value type="double" value="0.12"/>
            <value type="double" value="0.13"/>
            <value type="double" value="0.15"/>
            <value type="double" value="1.0"/>
          </property>
          <property name="image-style" type="int" value="0"/>
        </property>
      </property>
    </property>
  </property>
</channel>
EOF

# Pin GTK and cursor settings — dark theme for all GTK apps
cat > "${XFCE_SESSION_DIR}/xsettings.xml" << 'EOF'
<?xml version="1.0" encoding="UTF-8"?>
<channel name="xsettings" version="1.0">
  <property name="Net" type="empty">
    <property name="ThemeName" type="string" value="Adwaita-dark"/>
    <property name="IconThemeName" type="string" value="Adwaita"/>
  </property>
  <property name="Gtk" type="empty">
    <property name="CursorThemeName" type="string" value="Adwaita"/>
    <property name="CursorThemeSize" type="int" value="24"/>
  </property>
</channel>
EOF

# Xfdesktop normally uses a translucent rubber band. Keep an opaque fallback
# for AvoryOS&apos;s non-composited X11 path, where alpha fills may disappear.
mkdir -p "${ROOTFS_DIR}/etc/xdg/gtk-3.0"
cat > "${ROOTFS_DIR}/etc/xdg/gtk-3.0/gtk.css" << 'EOF'
XfdesktopIconView .rubberband,
XfdesktopIconView rubberband {
    background-color: #3584e4;
    border: 1px solid #1c71d8;
    border-radius: 0;
}
EOF

mkdir -p "${ROOTFS_DIR}/etc/lightdm" "${ROOTFS_DIR}/etc/X11/xinit" "${ROOTFS_DIR}/etc/pam.d"
cat > "${ROOTFS_DIR}/etc/lightdm/lightdm.conf" << 'EOF'
[LightDM]
run-directory=/run/lightdm
start-default-seat=true
logind-load-seats=false
logind-check-graphical=false

[Seat:*]
type=local
greeter-session=lightdm-gtk-greeter
greeter-hide-users=false
user-session=xfce
xserver-command=/usr/libexec/Xorg -noreset -nolisten tcp
session-wrapper=/etc/X11/xinit/Xsession
EOF

cat > "${ROOTFS_DIR}/etc/lightdm/lightdm-gtk-greeter.conf" << 'EOF'
[greeter]
at-spi-enabled=false
indicators=~host;~spacer;~clock;~power
theme-name=Adwaita
icon-theme-name=Adwaita
EOF

# Ensure PAM services for LightDM (greeter, login, autologin) work without elogind / utmps issues
cat > "${ROOTFS_DIR}/etc/pam.d/lightdm-greeter" << 'EOF'
#%PAM-1.0
auth      required  pam_permit.so
account   required  pam_permit.so
password  required  pam_deny.so
session   required  pam_env.so
session   required  pam_unix.so
-session  optional  pam_limits.so
EOF

cat > "${ROOTFS_DIR}/etc/pam.d/lightdm" << 'EOF'
#%PAM-1.0
auth      required  pam_permit.so
account   required  pam_permit.so
password  required  pam_deny.so
session   required  pam_env.so
session   required  pam_unix.so
-session  optional  pam_limits.so
EOF

cat > "${ROOTFS_DIR}/etc/pam.d/lightdm-autologin" << 'EOF'
#%PAM-1.0
auth      required  pam_permit.so
account   required  pam_permit.so
password  required  pam_deny.so
session   required  pam_env.so
session   required  pam_unix.so
-session  optional  pam_limits.so
EOF

# Ensure base-session-noninteractive does not fail if pam_limits is missing libutmps
if [ -f "${ROOTFS_DIR}/etc/pam.d/base-session-noninteractive" ]; then
    sed -i 's/^session[[:space:]]\+required[[:space:]]\+pam_limits.so/-session optional pam_limits.so/' \
        "${ROOTFS_DIR}/etc/pam.d/base-session-noninteractive" 2>/dev/null || true
fi
cat > "${ROOTFS_DIR}/etc/xprofile" << 'EOF'
export XDG_SESSION_TYPE=x11
export XDG_CURRENT_DESKTOP=XFCE
export XDG_SESSION_DESKTOP=xfce
export DESKTOP_SESSION=xfce
export XDG_CONFIG_DIRS=/etc/xdg:/etc
export XDG_DATA_DIRS=/usr/local/share:/usr/share
export GDK_GL=disable
export LIBGL_DRI3_DISABLE=1
export NO_AT_BRIDGE=1
export GTK_A11Y=none
export GIO_USE_VFS=local
export GIO_USE_VOLUME_MONITOR=unix
export GTK_USE_PORTAL=0
export QT_NO_PORTAL=1
EOF

# 3c. Do not patch xfce4-about in place.  Even a one-byte change in an ELF
# executable invalidates its section offsets; branding belongs in os-release
# and desktop metadata, not in a compiled third-party binary.
XFCE_ABOUT_DESKTOP="${ROOTFS_DIR}/usr/share/applications/xfce4-about.desktop"
if [ -f "${XFCE_ABOUT_DESKTOP}" ]; then
    sed -i 's/^Name=About Xfce$/Name=About AvoryOS/' "${XFCE_ABOUT_DESKTOP}"
    sed -i 's/^Comment=Information about the Xfce Desktop Environment$/Comment=Information about the AvoryOS desktop environment/' "${XFCE_ABOUT_DESKTOP}"
fi

# 3d. Brand the assembled system while retaining accurate userland attribution.
echo "[*] Writing AvoryOS operating-system identity..."
mkdir -p "${ROOTFS_DIR}/etc" "${ROOTFS_DIR}/usr/lib" "${ROOTFS_DIR}/usr/share/doc/avoryos"
rm -f "${ROOTFS_DIR}/etc/os-release" "${ROOTFS_DIR}/usr/lib/os-release"
cat > "${ROOTFS_DIR}/usr/lib/os-release" <<'EOF'
NAME="AvoryOS"
ID=avoryos
VERSION="2.0.0 Beta"
VERSION_ID="2.0.0-beta"
PRETTY_NAME="AvoryOS 2.0.0 Beta x86_64"
HOME_URL="https://github.com/Hidotu-Labs/AvoryOS"
SUPPORT_URL="https://github.com/Hidotu-Labs/AvoryOS"
BUG_REPORT_URL="https://github.com/Hidotu-Labs/AvoryOS"
EOF
ln -s ../usr/lib/os-release "${ROOTFS_DIR}/etc/os-release"
cat > "${ROOTFS_DIR}/usr/share/doc/avoryos/ALPINE-USERLAND" <<'EOF'
AvoryOS includes a userland assembled from Alpine Linux packages.
Alpine Linux is an independent project and does not produce or endorse AvoryOS.
The copyright notices and license terms shipped with each package continue to apply.
See /usr/share/licenses and the corresponding package metadata where available.
EOF

# 4. Create weston.ini
echo "[*] Creating /etc/weston.ini..."
mkdir -p "${ROOTFS_DIR}/etc"
mkdir -p "${ROOTFS_DIR}/usr/share/icons/default"
cat > "${ROOTFS_DIR}/usr/share/icons/default/index.theme" <<'EOF'
[Icon Theme]
Name=Default
Inherits=Adwaita
EOF
rm -rf "${ROOTFS_DIR}/usr/share/icons/default/cursors"
ln -s ../Adwaita/cursors "${ROOTFS_DIR}/usr/share/icons/default/cursors"

cat > "${ROOTFS_DIR}/etc/weston.ini" <<EOF
[core]
backend=drm-backend.so
shell=desktop-shell.so
xwayland=true

# Pointer acceleration is handed to libinput.  In a VM the host already
# accelerates the pointer before QEMU forwards the motion, so keep the guest
# at a constant speed with the flat profile instead of accelerating quick
# movements a second time (factor = 1 + accel-speed, 0.0 is 1:1).
# Tune accel-speed between -1.0 (slowest) and 0.0 (1:1).
[libinput]
accel-profile=flat
accel-speed=0.0

[shell]
panel-position=top
locking=false
background-image=/assets/room.png
background-type=scale
cursor-theme=Adwaita
cursor-size=24

[launcher]
icon=/usr/share/weston/icon_terminal.png
path=/usr/bin/weston-terminal

[launcher]
icon=/usr/share/pixmaps/sddm.png
path=/usr/bin/sddm

[launcher]
icon=/usr/share/pixmaps/netsurf.xpm
path=/usr/bin/netsurf

[output]
name=HDMI-A-1
mode=preferred
EOF

# 4c. Setup helper symlinks in /usr/bin for path resolutions (e.g. tar child execs)
echo "[*] Creating helper symlinks in /usr/bin..."
mkdir -p "${ROOTFS_DIR}/usr/bin"
ln -sf /bin/busybox "${ROOTFS_DIR}/usr/bin/gzip"
ln -sf /bin/busybox "${ROOTFS_DIR}/usr/bin/tar"
ln -sf /bin/busybox "${ROOTFS_DIR}/usr/bin/xz"
ln -sf /bin/busybox "${ROOTFS_DIR}/usr/bin/unzip"
ln -sf /bin/busybox "${ROOTFS_DIR}/usr/bin/zip"

# 4d. Configure local users and groups without shadow databases
echo "[*] Configuring passwd/group databases (no shadow)..."
"${ROOT_DIR}/scripts/configure-accounts.sh" "${ROOTFS_DIR}"

# ── KDE PLASMA 6 Desktop Environment ─────────────────────────────────────────
# Plasma 6 runs natively on Wayland via kwin_wayland, or on X11 via kwin_x11.
# All packages are from the selected Alpine branch (${ALPINE_BRANCH} community).
echo "[*] Installing KDE Plasma 6 core..."

# --- KDE Frameworks 6 (KF6) runtime libraries ---
# These are shared by kwin, plasma-workspace and plasma-desktop.
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
# solid-libs carries libKF6Solid.so.6 (needed by libKF6KIOGui.so.6)
install_apk "solid-libs" "community"
# solid-libs links libimobiledevice and libplist for iOS device detection
install_apk "libplist" "community"
# libimobiledevice-glue carries libimobiledevice-glue-1.0.so.0 (needed by libusbmuxd)
install_apk "libimobiledevice-glue" "community"
# libusbmuxd carries libusbmuxd-2.0.so.7 (needed by libimobiledevice)
install_apk "libusbmuxd" "community"
install_apk "libimobiledevice" "community"
# kstatusnotifieritem carries libKF6StatusNotifierItem.so.6 (needed by kwalletmanager)
install_apk "kstatusnotifieritem" "community"
install_apk "networkmanager" "community"
install_apk "networkmanager-wifi" "community"
install_apk "networkmanager-tui" "community"
# libKF6NetworkManagerQt.so.6 (needed by kded networkmanagement + plasmavault modules)
install_apk "networkmanager-qt" "community"
install_apk "modemmanager" "community"
# libKF6ModemManagerQt.so.6 (needed by kded networkmanagement module)
install_apk "modemmanager-qt" "community"
install_apk "mobile-broadband-provider-info" "community"

# --- Plasma 6 infrastructure ---
install_apk "kdecoration" "community"
install_apk "layer-shell-qt" "community"
install_apk "libkscreen" "community"
install_apk "libksysguard" "community"
# libsensors.so.5 (needed by libKSysGuardSystemStats.so.2)
install_apk "lm-sensors-libs" "main"
# libnl-3.so.200 and libpcap.so.1 (needed by libksysguard network/process stats)
install_apk "libnl3" "main"
install_apk "libpcap" "main"
install_apk "plasma5support" "community"
install_apk "plasma-activities" "community"
install_apk "plasma-activities-libs" "community"
install_apk "plasma-activities-stats" "community"

# --- kwin hard runtime dependencies ---
# libKF6Svg.so.6
install_apk "ksvg" "community"
# libKWaylandClient.so.6
install_apk "kwayland" "community"
# libKGlobalAccelD.so.0
install_apk "kglobalacceld" "community"
# libKScreenLocker.so.6
install_apk "kscreenlocker" "community"
# libQt6Sensors.so.6
install_apk "qt6-qtsensors" "community" "${QT6_BRANCH}"
# libqaccessibilityclient-qt6.so.0
install_apk "libqaccessibilityclient" "community"

# --- kwin QML/plugin runtime deps ---
# breeze carries org.kde.breeze decoration plugin
install_apk "breeze-cursors" "community"
install_apk "breeze" "community"
# frameworkintegration carries libKF6Style.so.6 (required by breeze6.so Qt6 style plugin)
install_apk "frameworkintegration" "community"
# qqc2-desktop-style and qqc2-breeze-style provide native styling for QtQuick Controls 2 / QML
install_apk "qqc2-desktop-style" "community"
install_apk "qqc2-breeze-style" "community"
# kirigami-libs carries libKirigami2.so (QML C++ backend)
install_apk "kirigami-libs" "community"
install_apk "kirigami" "community"
# kirigami-addons provides org.kde.kirigamiaddons.formcard and org.kde.kirigamiaddons.components (needed by Kickoff and Plasma Welcome)
install_apk "kirigami-addons" "community"
# kpipewire carries the screencast plugin
install_apk "pipewire" "community"
install_apk "kpipewire" "community"
# PipeWire audio infrastructure
install_apk "pipewire-libs" "community"
install_apk "pipewire-pulse" "community"
install_apk "wireplumber" "community"
install_apk "wireplumber-libs" "community"
install_apk "pulseaudio-utils" "community"
# libKF6PulseAudioQt.so.5 (needed by kded audioshortcutsservice module)
install_apk "pulseaudio-qt" "community"
install_apk "avahi-libs" "main"
install_apk "bluez" "main"
install_apk "bluez-libs" "main"
# qt6-qtshadertools carries libQt6ShaderTools.so.6
install_apk "qt6-qtshadertools" "community" "${QT6_BRANCH}"
# qt6-qt5compat carries GraphicalEffects QML plugin
install_apk "icu-data-full" "main"
install_apk "qt6-qt5compat" "community" "${QT6_BRANCH}"

# --- KWin (Wayland + X11 compositor / window manager) ---
install_apk "kwin" "community"

# --- Core Plasma workspace shell (includes startplasma-x11, startplasma-wayland) ---
install_apk "plasma-workspace" "community"
install_apk "plasma-workspace-libs" "community"
install_apk "plasma-workspace-wallpapers" "community"
# X11 session support for startplasma-x11
install_apk "plasma-workspace-x11" "community"

# --- Plasma desktop shell ---
install_apk "plasma-desktop" "community"

# --- Plasma system components ---
install_apk "bluedevil" "community"
# libKF6BluezQt.so.6 (needed by kded bluedevil module)
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

# --- Plasma addons ---
install_apk "kdeplasma-addons" "community"
install_apk "libplasma" "community"

# --- KDE applications commonly expected in a Plasma session ---
# dolphin and dependencies
install_apk "musl-fts" "main"
install_apk "lmdb" "main"
install_apk "dolphin" "community"
# konsole and dependencies
install_apk "knotifyconfig" "community"
install_apk "attica" "community"
install_apk "syndication" "community"
install_apk "konsole" "community"
# kate and dependencies
install_apk "syntax-highlighting" "community"
install_apk "ktexteditor" "community"
install_apk "kuserfeedback" "community"
install_apk "kate-common" "community"
install_apk "kate" "community"
# kdbusaddons
install_apk "kdbusaddons" "community"
install_apk "kfind" "community"
# ark and dependencies
install_apk "kfilemetadata" "community"
install_apk "libarchive" "main"
install_apk "libzip" "community"
install_apk "lrzip" "community"
install_apk "unzip" "main"
install_apk "zip" "main"
install_apk "zstd" "main"
install_apk "ark" "community"
# okular and dependencies
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
install_apk "kdegraphics-mobipocket" "community"
install_apk "libqca-qt6" "community"
install_apk "discount" "community"
install_apk "hunspell" "main"
install_apk "sonnet" "community"
install_apk "okular" "community"
install_apk "spectacle" "community"
# gwenview and dependencies
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

# --- SDDM display manager ---
echo "[*] Installing SDDM display manager..."
install_apk "sddm" "community"
install_apk "sddm-breeze" "community"
install_apk "qt6-qtdeclarative" "community" "${QT6_BRANCH}"
# libQt6Positioning.so.6 (needed by kded colorcorrectlocationupdater module)
install_apk "qt6-qtpositioning" "community" "${QT6_BRANCH}"
install_apk "qt6-qtimageformats" "community" "${QT6_BRANCH}"
install_apk "qt6-qtmultimedia" "community" "${QT6_BRANCH}"
install_apk "qt6-qtnetworkauth" "community" "${QT6_BRANCH}"
install_apk "qt6-qtvirtualkeyboard" "community" "${QT6_BRANCH}"
# libQt6Bluetooth.so.6 (needed by kdeconnect / org.kde.kdeconnect)
install_apk "qt6-qtconnectivity" "community" "${QT6_BRANCH}"

# --- KDE Frameworks 6 extras ---
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
# libqsqlite.so (QSQLITE database driver for kactivitymanagerd & plasma-activities-stats)
install_apk "qt6-qtbase-sqlite" "community" "${QT6_BRANCH}"

# --- Additional KDE Plasma 6 runtime dependencies ---
# libAppStreamQt.so.3 (needed by kickoff launcher kicker plugin)
install_apk "appstream-qt" "community"
# libKExiv2Qt6.so.0 (needed by wallpaper image plugin)
install_apk "libkexiv2" "community"
# libKF6UnitConversion.so.6 (needed by weather plugin)
install_apk "kunitconversion" "community"
# libQCoro6DBus.so.0 (needed by brightness control & plasma-nm)
install_apk "qcoro" "community"
# libnm.so.0 (needed by plasma-nm)
install_apk "libnm" "community"
# libpolkit-qt6-core-1.so.1 (needed by plasma-localegen-helper)
install_apk "polkit-qt6" "community"
install_apk "kdeclarative" "community"
install_apk "oxygen" "community"
install_apk "xdg-utils" "community"
install_apk "desktop-file-utils" "community"
# libKF6Prison.so.6 (needed by libklipper / org.kde.plasma.clipboard)
install_apk "libdmtx-libs" "community"
install_apk "libdmtx" "community"
install_apk "zxing-cpp" "community"
install_apk "libqrencode" "community"
install_apk "prison" "community"

# 5. Inject custom binaries
echo "[*] Injecting custom binaries into rootfs..."
mkdir -p "${ROOTFS_DIR}/bin"
if [ -f "${ROOT_DIR}/userland/gtk_test.elf" ]; then
    cp "${ROOT_DIR}/userland/gtk_test.elf" "${ROOTFS_DIR}/bin/gtk_test"
    chmod +x "${ROOTFS_DIR}/bin/gtk_test"
fi
if [ -f "${ROOT_DIR}/userland/gtk3_test.elf" ]; then
    cp "${ROOT_DIR}/userland/gtk3_test.elf" "${ROOTFS_DIR}/bin/gtk3_test"
    chmod +x "${ROOTFS_DIR}/bin/gtk3_test"
fi
if [ -f "${ROOT_DIR}/userland/sdl3_test.elf" ]; then
    cp "${ROOT_DIR}/userland/sdl3_test.elf" "${ROOTFS_DIR}/bin/sdl3_test"
    chmod +x "${ROOTFS_DIR}/bin/sdl3_test"
fi

# Generate caches last: no subsequent rootfs customization may make their
# configuration or source-directory metadata stale before image population.
echo "[*] Building final Fontconfig caches for the target rootfs..."
TARGET_LOADER="${ROOTFS_DIR}/lib/ld-musl-x86_64.so.1"
TARGET_FC_CACHE="${ROOTFS_DIR}/usr/bin/fc-cache"
if [ -x "${TARGET_LOADER}" ] && [ -x "${TARGET_FC_CACHE}" ]; then
    mkdir -p "${ROOTFS_DIR}/var/cache/fontconfig"
    "${TARGET_LOADER}" \
        --library-path "${ROOTFS_DIR}/lib:${ROOTFS_DIR}/usr/lib" \
        "${TARGET_FC_CACHE}" --sysroot="${ROOTFS_DIR}" --really-force \
        --system-only
fi

# 5. Inject into disk.img if present
if [ -f "${DISK_IMG}" ]; then
    echo "[*] Extracting partition 1 from disk.img..."
    PART_IMG="${BUILD_DIR}/part1.img"
    dd if="${DISK_IMG}" of="${PART_IMG}" bs=1M skip=1 status=none

    echo "[*] Populating partition with Alpine rootfs (using debugfs)..."
    "${POPULATE_SCRIPT}" "${PART_IMG}" "${ROOTFS_DIR}" "/"

    echo "[*] Re-injecting partition 1 into disk.img..."
    dd if="${PART_IMG}" of="${DISK_IMG}" bs=1M seek=1 conv=notrunc status=none

    echo "[SUCCESS] Alpine rootfs installed into ${DISK_IMG}"
else
    echo "[SUCCESS] Alpine rootfs prepared in ${ROOTFS_DIR}"
fi

mkdir -p "${BUILD_DIR}"
touch "${BUILD_DIR}/.built"

