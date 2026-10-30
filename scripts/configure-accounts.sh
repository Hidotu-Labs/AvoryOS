#!/bin/sh
set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT_DIR=$(dirname "$SCRIPT_DIR")
ROOTFS_DIR=${1:?usage: configure-accounts.sh ROOTFS_DIR}

DEFAULT_SHELL="/bin/bash"

# AvoryOS uses BusyBox init rather than systemd as PID 1. Replace Arch's
# timedate1 D-Bus activation entry (which runs /bin/false and delegates to
# systemd) with direct activation of systemd-timedated. Provision this into the
# image so boot does not need a shell here-document to rewrite the service.
mkdir -p "${ROOTFS_DIR}/usr/share/dbus-1/system-services"
install -m 0644 "${ROOT_DIR}/config/dbus/org.freedesktop.timedate1.service" \
    "${ROOTFS_DIR}/usr/share/dbus-1/system-services/org.freedesktop.timedate1.service"

# /tmp is mounted fresh at boot, hiding directories from the rootfs image.
# KWin's Xwayland launcher requires these shared socket directories to exist;
# rcS calls avory-early-init before starting desktop services. Preserve other
# early-init hooks and replace this block when provisioning an existing tree.
mkdir -p "${ROOTFS_DIR}/sbin"
if [ ! -f "${ROOTFS_DIR}/sbin/avory-early-init" ]; then
    printf '#!/bin/sh\n' > "${ROOTFS_DIR}/sbin/avory-early-init"
fi
sed -i '/^# BEGIN avory desktop sockets$/,/^# END avory desktop sockets$/d' \
    "${ROOTFS_DIR}/sbin/avory-early-init"
cat >> "${ROOTFS_DIR}/sbin/avory-early-init" << 'SOCKETS_EOF'
# BEGIN avory desktop sockets
mkdir -p /tmp/.X11-unix /tmp/.ICE-unix
chmod 1777 /tmp/.X11-unix /tmp/.ICE-unix
# END avory desktop sockets
SOCKETS_EOF
chmod 0755 "${ROOTFS_DIR}/sbin/avory-early-init"

mkdir -p "${ROOTFS_DIR}/etc" "${ROOTFS_DIR}/home" "${ROOTFS_DIR}/etc/skel"
awk -F: -v shell="${DEFAULT_SHELL}" 'BEGIN { OFS = ":" }
    $2 == "x" { $2 = "!" }
    $1 == "root" { $2 = "x"; $3 = 0; $4 = 0; $6 = "/"; $7 = shell; root = 1 }
    { print }
    END { if (!root) print "root", "x", 0, 0, "root", "/", shell }
' "${ROOTFS_DIR}/etc/passwd" > "${ROOTFS_DIR}/etc/passwd.avory-new"
mv "${ROOTFS_DIR}/etc/passwd.avory-new" "${ROOTFS_DIR}/etc/passwd"
rm -f "${ROOTFS_DIR}/etc/shadow" "${ROOTFS_DIR}/etc/shadow-" \
      "${ROOTFS_DIR}/etc/gshadow" "${ROOTFS_DIR}/etc/gshadow-"

# Axon's rootfs install does not run systemd-sysusers. Provision the Polkit
# system account from the package's sysusers rule so its rules directory can
# retain the intended root:polkitd ownership in the ext4 image.
POLKIT_RULES_DIR="${ROOTFS_DIR}/etc/polkit-1/rules.d"
POLKIT_SYSUSERS="${ROOTFS_DIR}/usr/lib/sysusers.d/polkit.conf"
if [ -d "${POLKIT_RULES_DIR}" ]; then
    if [ ! -r "${POLKIT_SYSUSERS}" ]; then
        echo "Missing Polkit sysusers definition: ${POLKIT_SYSUSERS}" >&2
        exit 1
    fi

    POLKIT_GID=$(awk '$1 == "g" && $2 == "polkitd" { print $3; exit }' "${POLKIT_SYSUSERS}")
    POLKIT_USER_SPEC=$(awk '$1 == "u!" && $2 == "polkitd" { print $3; exit }' "${POLKIT_SYSUSERS}")
    POLKIT_UID=${POLKIT_USER_SPEC%%:*}
    POLKIT_USER_GROUP=${POLKIT_USER_SPEC#*:}
    case "${POLKIT_GID}:${POLKIT_UID}" in
        *[!0-9:]*|:*|*:)
            echo "Invalid Polkit sysusers definition: ${POLKIT_SYSUSERS}" >&2
            exit 1
            ;;
    esac
    if [ "${POLKIT_USER_GROUP}" != "polkitd" ]; then
        echo "Unexpected Polkit primary group '${POLKIT_USER_GROUP}'." >&2
        exit 1
    fi

    EXISTING_POLKIT_GID=$(awk -F: '$1 == "polkitd" { print $3; exit }' "${ROOTFS_DIR}/etc/group")
    if [ -n "${EXISTING_POLKIT_GID}" ]; then
        if [ "${EXISTING_POLKIT_GID}" != "${POLKIT_GID}" ]; then
            echo "Polkit group has GID ${EXISTING_POLKIT_GID}; expected ${POLKIT_GID}." >&2
            exit 1
        fi
    else
        CONFLICTING_GROUP=$(awk -F: -v id="${POLKIT_GID}" '$3 == id { print $1; exit }' "${ROOTFS_DIR}/etc/group")
        if [ -n "${CONFLICTING_GROUP}" ]; then
            echo "Polkit GID ${POLKIT_GID} is already used by group ${CONFLICTING_GROUP}." >&2
            exit 1
        fi
        printf 'polkitd:x:%s:\n' "${POLKIT_GID}" >> "${ROOTFS_DIR}/etc/group"
    fi

    EXISTING_POLKIT_IDS=$(awk -F: '$1 == "polkitd" { print $3 ":" $4; exit }' "${ROOTFS_DIR}/etc/passwd")
    if [ -n "${EXISTING_POLKIT_IDS}" ]; then
        if [ "${EXISTING_POLKIT_IDS}" != "${POLKIT_UID}:${POLKIT_GID}" ]; then
            echo "Polkit user has IDs ${EXISTING_POLKIT_IDS}; expected ${POLKIT_UID}:${POLKIT_GID}." >&2
            exit 1
        fi
    else
        CONFLICTING_USER=$(awk -F: -v id="${POLKIT_UID}" '$3 == id { print $1; exit }' "${ROOTFS_DIR}/etc/passwd")
        if [ -n "${CONFLICTING_USER}" ]; then
            echo "Polkit UID ${POLKIT_UID} is already used by user ${CONFLICTING_USER}." >&2
            exit 1
        fi
        printf 'polkitd:!:%s:%s:User for polkitd:/:/usr/bin/nologin\n' \
            "${POLKIT_UID}" "${POLKIT_GID}" >> "${ROOTFS_DIR}/etc/passwd"
    fi
fi

chmod 0644 "${ROOTFS_DIR}/etc/passwd" "${ROOTFS_DIR}/etc/group"
chmod 0755 "${ROOTFS_DIR}/home"
rm -rf "${ROOTFS_DIR}/root"
mkdir -p "${ROOTFS_DIR}/root/.config/alacritty" "${ROOTFS_DIR}/root/.cache" "${ROOTFS_DIR}/root/.local/share"
mkdir -p "${ROOTFS_DIR}/.config/alacritty" "${ROOTFS_DIR}/.cache" "${ROOTFS_DIR}/.local/share" "${ROOTFS_DIR}/.local/state"
mkdir -p "${ROOTFS_DIR}/dev/dri"
chmod 0755 "${ROOTFS_DIR}/dev/dri" 2>/dev/null || true
chmod 0777 "${ROOTFS_DIR}/.config" "${ROOTFS_DIR}/.cache" "${ROOTFS_DIR}/.local" 2>/dev/null || true

# Folder View defaults to desktop:/, which resolves through the user's XDG
# Desktop location. Seed real directories for the unusual root HOME=/ too.
# Keep existing user paths; new accounts inherit these defaults via /etc/skel.
for desktop_home in "${ROOTFS_DIR}" "${ROOTFS_DIR}/root" \
    "${ROOTFS_DIR}/home/avory" "${ROOTFS_DIR}/etc/skel"; do
    mkdir -p "$desktop_home/.config"
    # Seed default output layout so the visible monitor (HDMI-A-1) is enabled
    # while the headless VKMS test output (Virtual-1) remains disabled.
    if [ -f "${ROOT_DIR}/config/kwinoutputconfig.json" ]; then
        cp -f "${ROOT_DIR}/config/kwinoutputconfig.json" "$desktop_home/.config/kwinoutputconfig.json"
    fi
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
mkdir -p "${ROOTFS_DIR}/etc/xdg" \
    "${ROOTFS_DIR}/home/avory/.config" \
    "${ROOTFS_DIR}/home/avory/.cache" \
    "${ROOTFS_DIR}/home/avory/.local/share" \
    "${ROOTFS_DIR}/home/avory/.local/state"
cat > "${ROOTFS_DIR}/etc/xdg/kwinrc" << 'KWINRC_EOF'
[Compositing]
Backend=OpenGL
Enabled=true
GLCore=false
GLPlatformInterface=glx
GLPreferBufferSwap=a
OpenGLIsUnsafe=false

[Wayland]
AllowTearing=true
KWINRC_EOF
cp -f "${ROOTFS_DIR}/etc/xdg/kwinrc" "${ROOTFS_DIR}/.config/kwinrc"
cp -f "${ROOTFS_DIR}/etc/xdg/kwinrc" "${ROOTFS_DIR}/root/.config/kwinrc"
cp -f "${ROOTFS_DIR}/etc/xdg/kwinrc" "${ROOTFS_DIR}/home/avory/.config/kwinrc"

# startplasma-wayland does not source /etc/profile.d. Select the connected
# display before KWin starts, so its default layout cannot choose only the
# headless VKMS test output. Plasma sources this directory for direct and
# display-manager launches; avory-drm.sh preserves explicit GPU overrides.
mkdir -p "${ROOTFS_DIR}/etc/xdg/plasma-workspace/env"
cp -f "${ROOT_DIR}/initrd/avory-drm.sh" \
    "${ROOTFS_DIR}/etc/xdg/plasma-workspace/env/avory-drm.sh"
chmod 0644 "${ROOTFS_DIR}/etc/xdg/plasma-workspace/env/avory-drm.sh"

# Plasma's Wayland desktop entry bypasses startplasma-x11, so route it through
# the matching audio setup before Plasma starts D-Bus-activated services.
install -m 0755 "${ROOT_DIR}/config/plasma/avory-plasma-wayland.sh" \
    "${ROOTFS_DIR}/usr/bin/avory-plasma-wayland"
PLASMA_WAYLAND_SESSION="${ROOTFS_DIR}/usr/share/wayland-sessions/plasma.desktop"
if [ -f "$PLASMA_WAYLAND_SESSION" ]; then
    sed -i 's|^Exec=.*|Exec=/usr/bin/avory-plasma-wayland|' "$PLASMA_WAYLAND_SESSION"
fi

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
if [ "$USER_ID" != 0 ]; then
    ACCOUNT_HOME="$(awk -F: -v uid="$USER_ID" '$3 == uid { print $6; exit }' /etc/passwd 2>/dev/null)"
    if [ -z "${HOME:-}" ] || [ "$HOME" = / ] || [ "$HOME" = /root ]; then
        if [ -n "$ACCOUNT_HOME" ] && [ "$ACCOUNT_HOME" != / ]; then
            HOME="$ACCOUNT_HOME"
            export HOME
        fi
    fi
    [ "${XDG_CONFIG_HOME:-}" = /.config ] && unset XDG_CONFIG_HOME
    [ "${XDG_CACHE_HOME:-}" = /.cache ] && unset XDG_CACHE_HOME
    [ "${XDG_DATA_HOME:-}" = /.local/share ] && unset XDG_DATA_HOME
    [ "${XDG_STATE_HOME:-}" = /.local/state ] && unset XDG_STATE_HOME
fi
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

# rcS starts root's audio graph in /run/pipewire. Keep the desktop runtime
# separate, but point native and PulseAudio clients at those existing sockets.
# Export before D-Bus activation so activated services inherit the same paths.
if [ "$USER_ID" = 0 ]; then
    export PIPEWIRE_RUNTIME_DIR=/run/pipewire
    # WebKitGTK 2.54's bindPulse() asserts on a nonempty unix: PULSE_SERVER
    # (CStringView::fromUTF8 is passed a span without its NUL terminator).
    # libpulse supports a runtime directory directly and still uses native.
    export PULSE_RUNTIME_PATH=/run/pipewire/pulse
    # Remove the default inherited from an older session wrapper on re-entry.
    if [ "${PULSE_SERVER:-}" = unix:/run/pipewire/pulse/native ]; then
        unset PULSE_SERVER
    fi
fi

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

# 5b. Non-root sessions need their own audio graph; root uses rcS above.
if [ "$USER_ID" != 0 ]; then
    session_pipewire_started=0
    if command -v pipewire >/dev/null 2>&1 && [ ! -S "$XDG_RUNTIME_DIR/pipewire-0" ]; then
        XDG_RUNTIME_DIR="$XDG_RUNTIME_DIR" nohup pipewire >>"$XDG_RUNTIME_DIR/pipewire.log" 2>&1 &
        session_pipewire_started=1
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
                if [ "$session_pipewire_started" = 1 ] || ! pidof wireplumber >/dev/null 2>&1; then
                    XDG_RUNTIME_DIR="$XDG_RUNTIME_DIR" nohup wireplumber >>"$XDG_RUNTIME_DIR/pipewire.log" 2>&1 &
                fi
            else
                XDG_RUNTIME_DIR="$XDG_RUNTIME_DIR" nohup wireplumber >>"$XDG_RUNTIME_DIR/pipewire.log" 2>&1 &
            fi
        fi
        if command -v pipewire-pulse >/dev/null 2>&1 && [ ! -S "$XDG_RUNTIME_DIR/pulse/native" ]; then
            nohup pipewire-pulse >>"$XDG_RUNTIME_DIR/pipewire.log" 2>&1 &
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
# from the environment (KWIN_COMPOSE exported by .xinitrc / the
# startplasma-x11 wrapper) and kwinrc, so no per-binary shim is needed.
# Unwrap any previous build's shim so a stale wrapper cannot survive a rebuild
# (install_arch skips re-extracting kwin once its marker exists).
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

# avory-login authenticates against /etc/shadow while running as root. Keep
# the root hash out of the world-readable passwd database, and regenerate its
# salt on each image build. Avory intentionally has an empty password.
if ! command -v openssl >/dev/null 2>&1 || ! command -v od >/dev/null 2>&1; then
    echo "openssl and od are required to create account password hashes." >&2
    exit 1
fi
make_password_hash() {
    password=$1
    salt=$(od -An -N8 -tx1 /dev/urandom | tr -d ' \n')
    case "$salt" in
        *[!0-9a-f]*|'')
            echo "Could not generate a password salt." >&2
            exit 1
            ;;
    esac
    printf '%s\n' "$password" | openssl passwd -6 -salt "$salt" -stdin
}
ROOT_PASSWORD_HASH=$(make_password_hash root)
case "$ROOT_PASSWORD_HASH" in '$6$'*) ;; *) echo "Could not create the root password hash." >&2; exit 1 ;; esac
# SDDM's unprivileged user model reads passwd, not shadow. An empty field
# advertises Avory's passwordless account to Breeze's Log In button.
awk -F: 'BEGIN { OFS = ":" } $1 == "root" { $2 = "x" } $1 == "avory" { $2 = "" } { print }' \
    "${ROOTFS_DIR}/etc/passwd" > "${ROOTFS_DIR}/etc/passwd.avory-new"
mv "${ROOTFS_DIR}/etc/passwd.avory-new" "${ROOTFS_DIR}/etc/passwd"
PASSWORD_SET_DAY=$(($(date +%s) / 86400))
(umask 077
 printf 'root:%s:%s::::::\navory::%s::::::\n' \
     "$ROOT_PASSWORD_HASH" "$PASSWORD_SET_DAY" \
     "$PASSWORD_SET_DAY" > "${ROOTFS_DIR}/etc/shadow")
chmod 0600 "${ROOTFS_DIR}/etc/shadow"
awk -F: 'NF != 9 { exit 1 }' "${ROOTFS_DIR}/etc/shadow" || {
    echo "Invalid /etc/shadow record: expected nine fields." >&2
    exit 1
}

# Let the default desktop account use the sudo package installed from Axon.
if [ -f "${ROOTFS_DIR}/etc/sudoers" ]; then
    if ! awk -F: '$1 == "wheel" { found = 1 } END { exit !found }' "${ROOTFS_DIR}/etc/group"; then
        echo "sudo is installed but its wheel group is missing from /etc/group." >&2
        exit 1
    fi
    awk -F: 'BEGIN { OFS = ":" } $1 == "wheel" {
        n = split($4, members, ",")
        present = 0
        for (i = 1; i <= n; i++) if (members[i] == "avory") present = 1
        if (!present) $4 = ($4 == "" ? "avory" : $4 ",avory")
    } { print }' "${ROOTFS_DIR}/etc/group" > "${ROOTFS_DIR}/etc/group.avory-new"
    mv "${ROOTFS_DIR}/etc/group.avory-new" "${ROOTFS_DIR}/etc/group"
    # The package ships sudoers read-only (0440). This is a build-time edit
    # inside the staged rootfs, so make it writable briefly and restore the
    # secure mode before the image is populated.
    chmod 0640 "${ROOTFS_DIR}/etc/sudoers"
    if ! sed -i 's/^# %wheel ALL=(ALL:ALL) ALL$/%wheel ALL=(ALL:ALL) ALL/' \
        "${ROOTFS_DIR}/etc/sudoers" ||
       ! grep -q '^%wheel ALL=(ALL:ALL) ALL$' "${ROOTFS_DIR}/etc/sudoers"; then
        echo "Could not enable the wheel rule in /etc/sudoers." >&2
        exit 1
    fi
    if ! grep -q '^Defaults:%wheel rootpw$' "${ROOTFS_DIR}/etc/sudoers"; then
        printf '\nDefaults:%%wheel rootpw\n' >> "${ROOTFS_DIR}/etc/sudoers"
    fi
    chmod 0440 "${ROOTFS_DIR}/etc/sudoers"
fi
chmod 0644 "${ROOTFS_DIR}/etc/passwd" "${ROOTFS_DIR}/etc/group"

# BusyBox init does not run the packages' systemd-sysusers units. Arch's
# system bus uses "dbus" (not the legacy messagebus account), and resolved
# drops privileges to systemd-resolve before creating its runtime sockets.
for service_user in dbus systemd-resolve; do
    service_gid=$(awk -F: -v name="$service_user" '$1 == name { print $3; exit }' "${ROOTFS_DIR}/etc/group")
    if [ -z "$service_gid" ]; then
        service_gid=$(awk -F: '{ used[$3] = 1 } END {
            for (id = 999; id >= 101; id--) if (!used[id]) { print id; exit }
            exit 1
        }' "${ROOTFS_DIR}/etc/group")
        printf '%s:x:%s:\n' "$service_user" "$service_gid" >> "${ROOTFS_DIR}/etc/group"
    fi
    if ! grep -q "^${service_user}:" "${ROOTFS_DIR}/etc/passwd"; then
        service_uid=$(awk -F: -v preferred="$service_gid" '{ used[$3] = 1 } END {
            if (!used[preferred]) { print preferred; exit }
            for (id = 999; id >= 101; id--) if (!used[id]) { print id; exit }
            exit 1
        }' "${ROOTFS_DIR}/etc/passwd")
        printf '%s:!:%s:%s:%s:/:/usr/bin/nologin\n' \
            "$service_user" "$service_uid" "$service_gid" "$service_user" >> "${ROOTFS_DIR}/etc/passwd"
    fi
done
unset service_user service_uid service_gid

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
