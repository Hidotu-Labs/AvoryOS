#!/bin/sh
set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT_DIR=$(dirname "$SCRIPT_DIR")
ROOTFS_DIR=${1:?usage: configure-accounts.sh ROOTFS_DIR}

DEFAULT_SHELL="/bin/sh"
if [ -f "${ROOTFS_DIR}/opt/bash/bin/bash" ] || [ -f "${ROOT_DIR}/toolchain/glibc-sysroot/opt/bash/bin/bash" ]; then
    DEFAULT_SHELL="/bin/bash"
fi

mkdir -p "${ROOTFS_DIR}/etc" "${ROOTFS_DIR}/home" "${ROOTFS_DIR}/etc/skel"
sed -E "s/^([^:]+):x:/\1:!:/" "${ROOTFS_DIR}/etc/passwd" | \
    sed -E "s#^root:[^:]*:0:0:([^:]*):[^:]*:.*#root::0:0:\1:/:${DEFAULT_SHELL}#" > \
    "${ROOTFS_DIR}/etc/passwd.avory-new"
mv "${ROOTFS_DIR}/etc/passwd.avory-new" "${ROOTFS_DIR}/etc/passwd"
rm -f "${ROOTFS_DIR}/etc/shadow" "${ROOTFS_DIR}/etc/shadow-" \
      "${ROOTFS_DIR}/etc/gshadow" "${ROOTFS_DIR}/etc/gshadow-"
chmod 0644 "${ROOTFS_DIR}/etc/passwd" "${ROOTFS_DIR}/etc/group"
chmod 0755 "${ROOTFS_DIR}/home"
rm -rf "${ROOTFS_DIR}/root"
mkdir -p "${ROOTFS_DIR}/root/.config/alacritty" "${ROOTFS_DIR}/root/.cache" "${ROOTFS_DIR}/root/.local/share"
mkdir -p "${ROOTFS_DIR}/.config/alacritty" "${ROOTFS_DIR}/.cache" "${ROOTFS_DIR}/.local/share" "${ROOTFS_DIR}/.local/state"
chmod 0777 "${ROOTFS_DIR}/.config" "${ROOTFS_DIR}/.cache" "${ROOTFS_DIR}/.local" 2>/dev/null || true

# Folder View defaults to desktop:/, which resolves through the user's XDG
# Desktop location. Seed real directories for the unusual root HOME=/ too.
# Keep existing user paths; new accounts inherit these defaults via /etc/skel.
for desktop_home in "${ROOTFS_DIR}" "${ROOTFS_DIR}/root" \
    "${ROOTFS_DIR}/home/avory" "${ROOTFS_DIR}/etc/skel"; do
    mkdir -p "$desktop_home/.config"
    for desktop_dir in Desktop Downloads Templates Public Documents Music Pictures Videos; do
        mkdir -p "$desktop_home/$desktop_dir"
    done
    if [ ! -e "$desktop_home/.config/user-dirs.dirs" ]; then
        cat > "$desktop_home/.config/user-dirs.dirs" << 'USER_DIRS_EOF'
XDG_DESKTOP_DIR="$HOME/Desktop"
XDG_DOWNLOAD_DIR="$HOME/Downloads"
XDG_TEMPLATES_DIR="$HOME/Templates"
XDG_PUBLICSHARE_DIR="$HOME/Public"
XDG_DOCUMENTS_DIR="$HOME/Documents"
XDG_MUSIC_DIR="$HOME/Music"
XDG_PICTURES_DIR="$HOME/Pictures"
XDG_VIDEOS_DIR="$HOME/Videos"
USER_DIRS_EOF
    fi
    if [ "$desktop_home" = "${ROOTFS_DIR}" ]; then
        # Qt expands $HOME literally: HOME=/ plus /Desktop becomes
        # //Desktop, and QUrl interprets that as file://desktop (no path).
        # Normalize root's paths, including defaults left by older images.
        sed -i 's#="\$HOME/#="/#; s#="//#="/#' \
            "$desktop_home/.config/user-dirs.dirs"
    fi
done

# Enable KWin compositing by default. KWIN_COMPOSE can still be overridden for
# systems whose graphics stack needs a different OpenGL presentation mode.
mkdir -p "${ROOTFS_DIR}/etc/xdg" "${ROOTFS_DIR}/home/avory/.config"
cat > "${ROOTFS_DIR}/etc/xdg/kwinrc" << 'KWINRC_EOF'
[Compositing]
Backend=OpenGL
Enabled=true
GLCore=false
GLPlatformInterface=glx
GLPreferBufferSwap=a
OpenGLIsUnsafe=false
KWINRC_EOF
cp -f "${ROOTFS_DIR}/etc/xdg/kwinrc" "${ROOTFS_DIR}/.config/kwinrc"
cp -f "${ROOTFS_DIR}/etc/xdg/kwinrc" "${ROOTFS_DIR}/root/.config/kwinrc"
cp -f "${ROOTFS_DIR}/etc/xdg/kwinrc" "${ROOTFS_DIR}/home/avory/.config/kwinrc"

# Xfwm4 ships with compositing enabled upstream, but write the channel default
# explicitly so fresh AvoryOS accounts get the same behavior.
XFWM4_CONFIG_DIR="${ROOTFS_DIR}/etc/xdg/xfce4/xfconf/xfce-perchannel-xml"
mkdir -p "$XFWM4_CONFIG_DIR"
cat > "$XFWM4_CONFIG_DIR/xfwm4.xml" << 'XFWM4_EOF'
<?xml version="1.0" encoding="UTF-8"?>
<channel name="xfwm4" version="1.0">
  <property name="general" type="empty">
    <property name="use_compositing" type="bool" value="true"/>
  </property>
</channel>
XFWM4_EOF
for user_config in \
    "${ROOTFS_DIR}/root/.config/xfce4/xfconf/xfce-perchannel-xml" \
    "${ROOTFS_DIR}/home/avory/.config/xfce4/xfconf/xfce-perchannel-xml"; do
    mkdir -p "$user_config"
    cp -f "$XFWM4_CONFIG_DIR/xfwm4.xml" "$user_config/xfwm4.xml"
done

# Symlink Mesa DRI drivers into /usr/lib/dri for default loader discovery
if [ -d "${ROOTFS_DIR}/usr/lib/xorg/modules/dri" ]; then
    mkdir -p "${ROOTFS_DIR}/usr/lib/dri"
    for drv in "${ROOTFS_DIR}/usr/lib/xorg/modules/dri"/*.so*; do
        [ -e "$drv" ] || continue
        base=$(basename "$drv")
        ln -sf "/usr/lib/xorg/modules/dri/$base" "${ROOTFS_DIR}/usr/lib/dri/$base"
    done
fi

# Wrap /usr/bin/startplasma-x11 so direct execution (e.g. DISPLAY=:0 startplasma-x11)
# transparently launches dbus-run-session, sets up XDG environment, and connects properly.
if [ -f "${ROOTFS_DIR}/usr/bin/startplasma-x11" ]; then
    if file "${ROOTFS_DIR}/usr/bin/startplasma-x11" | grep -q "ELF"; then
        mv "${ROOTFS_DIR}/usr/bin/startplasma-x11" "${ROOTFS_DIR}/usr/bin/startplasma-x11-bin"
    fi
    cat > "${ROOTFS_DIR}/usr/bin/startplasma-x11" << 'WRAPPER_EOF'
#!/bin/sh
set -e

# 1. Wait for X server to be available if DISPLAY is set
if [ -n "$DISPLAY" ]; then
    disp_num="${DISPLAY#:}"
    disp_num="${disp_num%%.*}"
    sock="/tmp/.X11-unix/X${disp_num}"
    for i in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20 21 22 23 24 25 26 27 28 29 30; do
        if [ -S "$sock" ] || (command -v xset >/dev/null 2>&1 && xset q >/dev/null 2>&1); then
            break
        fi
        sleep 0.1
    done
fi

# 2. Setup XDG_RUNTIME_DIR
USER_ID="$(id -u 2>/dev/null || echo 0)"
if [ -z "$XDG_RUNTIME_DIR" ] || [ ! -d "$XDG_RUNTIME_DIR" ]; then
    if [ -d "/run/user/$USER_ID" ]; then
        export XDG_RUNTIME_DIR="/run/user/$USER_ID"
    else
        export XDG_RUNTIME_DIR="/tmp/runtime-$USER_ID"
    fi
fi
mkdir -p "$XDG_RUNTIME_DIR" 2>/dev/null || true
chmod 0700 "$XDG_RUNTIME_DIR" 2>/dev/null || true

# 3. Setup XDG directories and ensure they exist
if [ -z "$HOME" ] || [ "$HOME" = "/" ]; then
    export HOME="/"
    export XDG_CONFIG_HOME="/.config"
    export XDG_CACHE_HOME="/.cache"
    export XDG_DATA_HOME="/.local/share"
    export XDG_STATE_HOME="/.local/state"
    mkdir -p /.config /.cache /.local/share /.local/state /tmp 2>/dev/null || true
    chmod 0755 /.config /.cache /.local/share /.local/state 2>/dev/null || true
else
    export XDG_CONFIG_HOME="${XDG_CONFIG_HOME:-$HOME/.config}"
    export XDG_CACHE_HOME="${XDG_CACHE_HOME:-$HOME/.cache}"
    export XDG_DATA_HOME="${XDG_DATA_HOME:-$HOME/.local/share}"
    export XDG_STATE_HOME="${XDG_STATE_HOME:-$HOME/.local/state}"
    mkdir -p "$XDG_CONFIG_HOME" "$XDG_CACHE_HOME" "$XDG_DATA_HOME" "$XDG_STATE_HOME" 2>/dev/null || true
fi

# Initialize newly created accounts before Qt/KIO reads their desktop path.
if command -v xdg-user-dirs-update >/dev/null 2>&1; then
    xdg-user-dirs-update 2>/dev/null || true
fi
# The updater may serialize /Desktop back to $HOME/Desktop. Root's HOME=/
# needs absolute paths before Plasma's desktop:/ worker converts them to URLs.
if [ "$HOME" = / ] && [ -f "$XDG_CONFIG_HOME/user-dirs.dirs" ]; then
    # This is optional configuration repair. Under set -e an in-place edit
    # failure (permissions, read-only config or filesystem error) otherwise
    # exits the session wrapper and sends SDDM straight back to the greeter.
    if ! sed -i 's#="\$HOME/#="/#; s#="//#="/#' \
        "$XDG_CONFIG_HOME/user-dirs.dirs"; then
        echo "[startplasma] Could not normalize XDG user directories; continuing session startup." >&2
    fi
fi
export XDG_DATA_DIRS="${XDG_DATA_DIRS:-/usr/local/share:/usr/share}"
export XDG_CONFIG_DIRS="${XDG_CONFIG_DIRS:-/etc/xdg}"

# The PRIME display is configured by avory-prime-display. KScreen otherwise
# treats the synthetic AMD HDMI sink as a second monitor and enables it.
# A session-only KConfig default keeps that startup module from replacing the
# virtual-output layout; native single-GPU sessions keep normal KScreen policy.
if command -v xrandr >/dev/null 2>&1 &&
   xrandr --query 2>/dev/null | awk '$2 == "connected" && $1 ~ /^HDMI-[0-9]+-[0-9]+$/ {found=1} END {exit !found}'; then
    prime_config="$XDG_RUNTIME_DIR/avory-prime-config"
    if mkdir -p "$prime_config" &&
       printf '[Module-kscreen]\nautoload[$i]=false\n' > "$prime_config/kded5rc"; then
        export XDG_CONFIG_DIRS="$prime_config:$XDG_CONFIG_DIRS"
    fi
    if command -v avory-prime-display >/dev/null 2>&1; then
        avory-prime-display || true
    fi
fi

# This wrapper starts an X11 session. Give Qt and D-Bus-activated helpers
# (notably ksecretd) the same platform and the packaged Qt6 plugin directory.
# Do not set QT_PLUGIN_PATH globally: Qt5 applications have their own plugins.
export XDG_SESSION_TYPE=x11
export XDG_CURRENT_DESKTOP=KDE
export XDG_SESSION_DESKTOP=KDE
# The real Plasma launcher sets KDE_FULL_SESSION once startup is underway.
# plasma-workspace supplies plasma-applications.menu, not applications.menu.
# Select it before both D-Bus activation and the service-cache rebuild.
export XDG_MENU_PREFIX=plasma-
export QT_QPA_PLATFORM=xcb
if [ -f /usr/lib/qt6/plugins/platforms/libqxcb.so ]; then
    export QT_QPA_PLATFORM_PLUGIN_PATH=/usr/lib/qt6/plugins/platforms
fi

# 4. Mesa / OpenGL configuration
if [ -d /usr/lib/xorg/modules/dri ]; then
    export LIBGL_DRIVERS_PATH="/usr/lib/xorg/modules/dri:/usr/lib/dri${LIBGL_DRIVERS_PATH:+:$LIBGL_DRIVERS_PATH}"
fi
export KWIN_COMPOSE="${KWIN_COMPOSE:-O2}"

# 5. D-Bus session bus management
if [ -z "$DBUS_SESSION_BUS_ADDRESS" ]; then
    mkdir -p /run/dbus /var/run/dbus 2>/dev/null || true
    [ -e /var/run/dbus ] || ln -sf /run/dbus /var/run/dbus 2>/dev/null || true
    if [ -S /run/dbus/system_bus_socket ]; then
        export DBUS_SYSTEM_BUS_ADDRESS=unix:path=/run/dbus/system_bus_socket
    fi

    if command -v dbus-run-session >/dev/null 2>&1; then
        exec dbus-run-session -- "$0" "$@"
    fi
fi

if command -v dbus-update-activation-environment >/dev/null 2>&1; then
    dbus-update-activation-environment --all 2>/dev/null || true
fi

# Build the per-user KDE service registry after package/image changes. This
# discovers KIO workers and application launchers without a systemd user unit.
# Discard an empty registry produced by earlier wrappers without a menu prefix.
if command -v kbuildsycoca6 >/dev/null 2>&1; then
    kbuildsycoca6 --noincremental || true
fi

# 5b. PipeWire session instance (per-user; the rcS boot instance is
# root-owned at /run/pipewire and is not visible here). Start one only if
# nothing is listening in this session's runtime dir; never fail the login.
if command -v pipewire >/dev/null 2>&1 && [ ! -S "$XDG_RUNTIME_DIR/pipewire-0" ]; then
    XDG_RUNTIME_DIR="$XDG_RUNTIME_DIR" nohup pipewire >>"$XDG_RUNTIME_DIR/pipewire.log" 2>&1 &
    for i in 1 2 3 4 5 6 7 8 9 10; do
        [ -S "$XDG_RUNTIME_DIR/pipewire-0" ] && break
        sleep 0.5
    done
fi
if [ -S "$XDG_RUNTIME_DIR/pipewire-0" ]; then
    if command -v wireplumber >/dev/null 2>&1; then
        if command -v pidof >/dev/null 2>&1; then
            # Background only the daemon. `pidof ... || command &` would
            # background the entire list in a temporary shell; that shell
            # can exit after this wrapper execs Plasma and remain unreaped.
            if ! pidof wireplumber >/dev/null 2>&1; then
                XDG_RUNTIME_DIR="$XDG_RUNTIME_DIR" nohup wireplumber >>"$XDG_RUNTIME_DIR/pipewire.log" 2>&1 &
            fi
        else
            XDG_RUNTIME_DIR="$XDG_RUNTIME_DIR" nohup wireplumber >>"$XDG_RUNTIME_DIR/pipewire.log" 2>&1 &
        fi
    fi
fi

REAL_BIN="/usr/bin/startplasma-x11-bin"
if [ ! -x "$REAL_BIN" ]; then
    REAL_BIN="/usr/bin/startplasma-x11.real"
fi
# A stray xfce4-notifyd (D-Bus-activated before plasmashell claimed
# org.freedesktop.Notifications, or leftover from an XFCE session) makes
# Plasma show "Notification service not available / provided by Xfce Notify
# Daemon". Reclaim the name for plasmashell; never fail the login.
pkill -x xfce4-notifyd 2>/dev/null || true
exec "$REAL_BIN" "$@"
WRAPPER_EOF
    chmod 0755 "${ROOTFS_DIR}/usr/bin/startplasma-x11"
fi

# No wrapper for kwin_x11: it runs as the real binary. Compositing mode comes
# from the environment (KWIN_COMPOSE exported by .xinitrc / startx.sh /
# startplasma-x11 wrapper) and kwinrc, so no per-binary shim is needed.
# Unwrap any previous build's shim so a stale wrapper cannot survive a rebuild
# (install_apk skips re-extracting kwin once its marker exists).
if [ -f "${ROOTFS_DIR}/usr/bin/kwin_x11-bin" ]; then
    if ! file "${ROOTFS_DIR}/usr/bin/kwin_x11-bin" | grep -q "ELF"; then
        echo "[!] kwin_x11-bin is not an ELF binary, leaving wrapper in place" >&2
    elif file "${ROOTFS_DIR}/usr/bin/kwin_x11" | grep -q "ELF"; then
        rm -f "${ROOTFS_DIR}/usr/bin/kwin_x11-bin"
    else
        mv -f "${ROOTFS_DIR}/usr/bin/kwin_x11-bin" "${ROOTFS_DIR}/usr/bin/kwin_x11"
    fi
fi


if ! grep -q "^avory:" "${ROOTFS_DIR}/etc/group"; then
    echo "avory:x:1000:" >> "${ROOTFS_DIR}/etc/group"
fi
if ! grep -q "^avory:" "${ROOTFS_DIR}/etc/passwd"; then
    echo "avory::1000:1000:AvoryOS User:/home/avory:${DEFAULT_SHELL}" >> "${ROOTFS_DIR}/etc/passwd"
fi
if ! grep -q "^messagebus:" "${ROOTFS_DIR}/etc/group"; then
    echo "messagebus:x:86:" >> "${ROOTFS_DIR}/etc/group"
fi
if ! grep -q "^messagebus:" "${ROOTFS_DIR}/etc/passwd"; then
    echo "messagebus:!:86:86:D-Bus Message Bus User:/var/run/dbus:/bin/false" >> "${ROOTFS_DIR}/etc/passwd"
fi
if ! grep -q "^sddm:" "${ROOTFS_DIR}/etc/group"; then
    echo "sddm:x:642:" >> "${ROOTFS_DIR}/etc/group"
fi
if ! grep -q "^sddm:" "${ROOTFS_DIR}/etc/passwd"; then
    echo "sddm:!:642:642:SDDM daemon:/var/lib/sddm:/sbin/nologin" >> "${ROOTFS_DIR}/etc/passwd"
fi
# The Arch sddm package may have created the account via sysusers with a
# different id (currently 963); the entries above are only a fallback.
# Always chown with the ids actually present in the target tree.
SDDM_UID=$(awk -F: '/^sddm:/{print $3; exit}' "${ROOTFS_DIR}/etc/passwd" 2>/dev/null)
SDDM_GID=$(awk -F: '/^sddm:/{print $3; exit}' "${ROOTFS_DIR}/etc/group" 2>/dev/null)
[ -n "${SDDM_UID}" ] || SDDM_UID=642
[ -n "${SDDM_GID}" ] || SDDM_GID=642
mkdir -p "${ROOTFS_DIR}/var/lib/sddm/.config" "${ROOTFS_DIR}/var/lib/sddm/.cache" "${ROOTFS_DIR}/var/lib/sddm/.local/share" "${ROOTFS_DIR}/var/log/sddm" "${ROOTFS_DIR}/run/sddm"
touch "${ROOTFS_DIR}/var/lib/sddm/.config/sddm-greeter-qt6rc" "${ROOTFS_DIR}/var/lib/sddm/.config/sddm-greeter-qt5rc" 2>/dev/null || true
chown -R "${SDDM_UID}:${SDDM_GID}" "${ROOTFS_DIR}/var/lib/sddm" "${ROOTFS_DIR}/var/log/sddm" "${ROOTFS_DIR}/run/sddm" 2>/dev/null || true
chmod -R 0775 "${ROOTFS_DIR}/var/lib/sddm" "${ROOTFS_DIR}/run/sddm" 2>/dev/null || true
chmod 0664 "${ROOTFS_DIR}/var/lib/sddm/.config/sddm-greeter-qt6rc" "${ROOTFS_DIR}/var/lib/sddm/.config/sddm-greeter-qt5rc" 2>/dev/null || true
cp -a "${ROOTFS_DIR}/etc/skel/." "${ROOTFS_DIR}/root/" 2>/dev/null || true
chmod 0700 "${ROOTFS_DIR}/root"

mkdir -p "${ROOTFS_DIR}/usr/sbin" "${ROOTFS_DIR}/bin"
cp "${ROOT_DIR}/userland/avory-account" "${ROOTFS_DIR}/usr/sbin/avory-account"
chmod 0755 "${ROOTFS_DIR}/usr/sbin/avory-account"
for command in useradd userdel usermod groupadd groupdel groupmod; do
    ln -sf /usr/sbin/avory-account "${ROOTFS_DIR}/usr/sbin/$command"
    ln -sf /usr/sbin/avory-account "${ROOTFS_DIR}/bin/$command"
done
cp "${ROOT_DIR}/userland/test_accounts.sh" "${ROOTFS_DIR}/bin/test_accounts"
chmod 0755 "${ROOTFS_DIR}/bin/test_accounts"
cp "${ROOT_DIR}/userland/avory-login.elf" "${ROOTFS_DIR}/bin/avory-login"
chmod 0755 "${ROOTFS_DIR}/bin/avory-login"

cat > "${ROOTFS_DIR}/etc/login.defs" <<EOF
UID_MIN 1000
UID_MAX 60000
GID_MIN 1000
GID_MAX 60000
CREATE_HOME yes
HOME_MODE 0700
UMASK 077
EOF
