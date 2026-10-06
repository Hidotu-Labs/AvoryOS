#!/bin/sh
# LightDM Display Manager startup script for AvoryOS
# Spawns system D-Bus, elogind (for seat management), and LightDM (autologin, no greeter).

export PATH="/usr/bin:/bin:/usr/local/bin:/opt/coreutils/bin:/opt/bash/bin:${PATH}"
export LD_LIBRARY_PATH="/usr/lib:/lib:/usr/local/lib:${LD_LIBRARY_PATH:-}"
# Enable KWin's OpenGL compositor by default. Users can still set
# KWIN_COMPOSE=N in the environment to disable it for troubleshooting.
# NOTE: KWin 6 dropped the XRender backend, so OpenGL/llvmpipe is the only
# X11 compositing path (Q is Wayland-only).
export KWIN_COMPOSE="${KWIN_COMPOSE:-O2}"
export NO_AT_BRIDGE=1
export GTK_A11Y=none
export WEBKIT_FORCE_SANDBOX=0
export GLYCIN_SANDBOX=none
export LIBRSVG_SANDBOX=none

# ── DRM card selection (Phase 6 C7) ────────────────────────────────────────
# Prefer amdgpu (card2) when it can drive a display, else the native card0.
# The modesetting driver reads the choice from its Device section, so rewrite
# the config before Xorg starts; the packaged 10-modesetting.conf stays as the
# static fallback for launchers that never run this script.
DRM_CARD="$(/bin/drm-pick.sh 2>/dev/null)"
[ -n "$DRM_CARD" ] || DRM_CARD=/dev/dri/card0
echo "[startx] DRM card: $DRM_CARD"
KMSDEV_OPT=""
KMS_BUSID_LINE=""
if [ "$DRM_CARD" != "/dev/dri/card0" ]; then
    KMSDEV_OPT="    Option      \"kmsdev\" \"$DRM_CARD\""
    # Xorg's modesetting driver only claims a PCI slot when the Device
    # section carries a BusID.  Without one, a kmsdev with no matching
    # platform device is claimed as an fb slot, and xf86PostProbe() fatals
    # ("Cannot run in framebuffer mode") once the native framebuffer is also
    # claimed.  Derive the amdgpu BusID from the PCI sysfs.
    for d in /sys/bus/pci/devices/*; do
        [ -r "$d/vendor" ] || continue
        [ "$(cat "$d/vendor" 2>/dev/null)" = "0x1002" ] || continue
        case "$(cat "$d/class" 2>/dev/null)" in
            0x03*) ;;
            *) continue ;;
        esac
        bdf="${d##*/}"
        KMS_BUSID_LINE="    BusID       \"PCI:$((16#${bdf:5:2})):$((16#${bdf:8:2})):$((16#${bdf:11:2}))\""
        break
    done
    # The passed GPU's guest BDF is fixed by the QEMU topology; keep it as a
    # fallback if the sysfs scan above found nothing.
    if [ -z "$KMS_BUSID_LINE" ] && [ -r /sys/bus/pci/devices/0000:00:03.0/vendor ]; then
        KMS_BUSID_LINE='    BusID       "PCI:0:3:0"'
    fi
fi
mkdir -p /etc/X11/xorg.conf.d
cat > /etc/X11/xorg.conf.d/10-modesetting.conf <<EOF
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
    Option      "AccelMethod" "glamor"
$KMSDEV_OPT
$KMS_BUSID_LINE
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

rm -f /tmp/.X0-lock /tmp/.X11-unix/X0

mkdir -p /var/log /var/lib/dbus /run/dbus /var/run \
         /run/sddm /var/lib/sddm /var/log/sddm \
         /run/systemd /run/user /root
[ -e /var/run/dbus ] || ln -sf /run/dbus /var/run/dbus 2>/dev/null || true
touch /var/run/utmp /var/log/wtmp /var/log/lastlog /run/utmp 2>/dev/null || true
chmod 0664 /var/run/utmp /var/log/wtmp /var/log/lastlog /run/utmp 2>/dev/null || true

# ── Dynamic account provisioning ──────────────────────────────────────────
# Internal system daemon account for SDDM greeter sandbox.
# The Arch package may own sddm under a different id via sysusers, so
# resolve instead of assuming: mismatched ownership locks the greeter
# session out of /var/lib/sddm (black screen, parked processes).
if ! grep -q "^sddm:" /etc/group 2>/dev/null; then
    echo "sddm:x:642:" >> /etc/group
fi
if ! grep -q "^sddm:" /etc/passwd 2>/dev/null; then
    echo "sddm:!:642:642:SDDM daemon:/var/lib/sddm:/sbin/nologin" >> /etc/passwd
fi
SDDM_UID="$(id -u sddm 2>/dev/null || echo 642)"
SDDM_GID="$(id -g sddm 2>/dev/null || echo 642)"
mkdir -p /var/lib/sddm/.config /var/lib/sddm/.cache /var/lib/sddm/.local/share \
         /run/sddm /var/log/sddm
touch /var/lib/sddm/.config/sddm-greeter-qt6rc /var/lib/sddm/.config/sddm-greeter-qt5rc 2>/dev/null || true
chown -R "$SDDM_UID:$SDDM_GID" /var/lib/sddm /run/sddm /var/log/sddm 2>/dev/null || true
chmod -R 0775 /var/lib/sddm /run/sddm /var/log/sddm 2>/dev/null || true
chmod 0664 /var/lib/sddm/.config/sddm-greeter-qt6rc /var/lib/sddm/.config/sddm-greeter-qt5rc 2>/dev/null || true

# ── D-Bus Initialization ──────────────────────────────────────────────────
if [ ! -s /etc/machine-id ]; then
    if command -v dbus-uuidgen >/dev/null 2>&1; then
        dbus-uuidgen --ensure=/etc/machine-id 2>/dev/null || true
    else
        echo "10000000000000000000000000000001" > /etc/machine-id
    fi
fi
[ -f /var/lib/dbus/machine-id ] || cp -f /etc/machine-id /var/lib/dbus/machine-id 2>/dev/null || true

# Ensure SDDM D-Bus policy is loaded in system bus configuration
mkdir -p /etc/dbus-1/system.d
[ -f /etc/dbus-1/system.d/org.freedesktop.DisplayManager.conf ] || \
    cp -f /usr/share/dbus-1/system.d/org.freedesktop.DisplayManager.conf /etc/dbus-1/system.d/ 2>/dev/null || true

# Userspace services (PipeWire, D-Bus activated apps, ...) derive their socket
# paths from XDG_RUNTIME_DIR.
XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-/tmp/runtime-0}"
export XDG_RUNTIME_DIR
mkdir -p "$XDG_RUNTIME_DIR" && chmod 1777 "$XDG_RUNTIME_DIR"

if command -v dbus-daemon >/dev/null 2>&1 && [ ! -S /run/dbus/system_bus_socket ]; then
    rm -f /tmp/ascent-system-dbus.log
    dbus-daemon --system --nofork >/tmp/ascent-system-dbus.log 2>&1 &
    SYSTEM_DBUS_PID=$!
    sleep 0.2
    if [ ! -S /run/dbus/system_bus_socket ] || ! kill -0 "$SYSTEM_DBUS_PID" 2>/dev/null; then
        echo "[startx] Warning: system D-Bus did not stay running."
        [ -s /tmp/ascent-system-dbus.log ] && cat /tmp/ascent-system-dbus.log
    fi
fi
export DBUS_SYSTEM_BUS_ADDRESS=unix:path=/var/run/dbus/system_bus_socket

if command -v dbus-daemon >/dev/null 2>&1; then
    rm -f /tmp/avory-session-bus
    dbus-daemon --session --fork --address=unix:path=/tmp/avory-session-bus 2>/dev/null || true
    export DBUS_SESSION_BUS_ADDRESS=unix:path=/tmp/avory-session-bus
    command -v dbus-update-activation-environment >/dev/null 2>&1 &&
        dbus-update-activation-environment --all 2>/dev/null || true
fi

chmod 1777 /tmp /tmp/.X11-unix 2>/dev/null || true

# ── Launch SDDM ───────────────────────────────────────────────────────────
if command -v sddm >/dev/null 2>&1; then
    echo "[startx] Launching SDDM display manager..."

    XORG_BIN="/usr/libexec/Xorg"
    [ -x "$XORG_BIN" ] || XORG_BIN="/usr/bin/Xorg"

    SDDM_THEME="elarun"
    if [ -d /usr/share/sddm/themes/breeze ]; then
        SDDM_THEME="breeze"
    fi

    # Make sure Xsession and Xsetup are executable
    if [ ! -f /usr/share/sddm/scripts/Xsetup ]; then
        mkdir -p /usr/share/sddm/scripts
        echo "#!/bin/sh" > /usr/share/sddm/scripts/Xsetup
    fi
    chmod +x /usr/share/sddm/scripts/Xsetup /usr/share/sddm/scripts/Xsession 2>/dev/null || true

    mkdir -p /etc/sddm.conf.d /etc
    cat > /etc/sddm.conf << SDDMCFG
[General]
DisplayServer=x11
HaltCommand=/sbin/poweroff
RebootCommand=/sbin/reboot
Numlock=none

[Theme]
Current=${SDDM_THEME}
ThemeDir=/usr/share/sddm/themes
FacesDir=/usr/share/sddm/faces
CursorTheme=Adwaita

[X11]
ServerPath=${XORG_BIN}
ServerArguments=-noreset -nolisten tcp -ac
SessionCommand=/usr/share/sddm/scripts/Xsession
SessionDir=/usr/share/xsessions
DisplayCommand=/usr/share/sddm/scripts/Xsetup
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
SDDMCFG
    # Ensure PAM services for SDDM greeter and user login permit root login
    mkdir -p /etc/pam.d
    cat > /etc/pam.d/sddm-greeter << 'PAMCFG'
#%PAM-1.0
auth      required  pam_env.so
auth      required  pam_permit.so
account   required  pam_permit.so
password  required  pam_deny.so
session   required  pam_unix.so
-session  optional  pam_elogind.so
PAMCFG

    cat > /etc/pam.d/sddm << 'PAMCFG'
#%PAM-1.0
auth      required  pam_permit.so
account   required  pam_permit.so
password  required  pam_deny.so
session   required  pam_env.so
session   required  pam_unix.so
-session  optional  pam_limits.so
-session  optional  pam_elogind.so
PAMCFG

    cat > /etc/pam.d/sddm-autologin << 'PAMCFG'
#%PAM-1.0
auth      required  pam_permit.so
account   required  pam_permit.so
password  required  pam_deny.so
session   required  pam_env.so
session   required  pam_unix.so
-session  optional  pam_limits.so
-session  optional  pam_elogind.so
PAMCFG

    sddm
    echo "=== /var/log/sddm.log ==="
    [ -f /var/log/sddm.log ] && cat /var/log/sddm.log
    echo "=== /.xsession-errors ==="
    [ -f /.xsession-errors ] && cat /.xsession-errors
    [ -f /root/.xsession-errors ] && cat /root/.xsession-errors
    exec /bin/sh
fi

echo "[startx] Error: sddm binary not found."
exec /bin/sh
