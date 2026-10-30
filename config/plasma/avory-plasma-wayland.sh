#!/bin/sh
# Plasma's Wayland desktop entry bypasses the startplasma-x11 wrapper. Set up
# the same PipeWire routing before starting D-Bus-activated Plasma services.

USER_ID="$(id -u 2>/dev/null || echo 0)"

# SDDM/session startup can inherit HOME=/ from AvoryOS's root login defaults.
# Plasma and its D-Bus services then try to write into /.cache and /.local,
# which are root-owned. Resolve the account home for non-root sessions before
# starting any desktop process.
if [ "$USER_ID" != 0 ]; then
    ACCOUNT_HOME="$(awk -F: -v uid="$USER_ID" '$3 == uid { print $6; exit }' /etc/passwd 2>/dev/null)"
    if [ -z "${HOME:-}" ] || [ "$HOME" = / ] || [ "$HOME" = /root ]; then
        if [ -n "$ACCOUNT_HOME" ] && [ "$ACCOUNT_HOME" != / ]; then
            HOME="$ACCOUNT_HOME"
            export HOME
        fi
    fi

    # Drop stale XDG paths that point at root's home. Keep intentional custom
    # paths, and derive the usual locations from this account's HOME below.
    [ "${XDG_CONFIG_HOME:-}" = /.config ] && unset XDG_CONFIG_HOME
    [ "${XDG_CACHE_HOME:-}" = /.cache ] && unset XDG_CACHE_HOME
    [ "${XDG_DATA_HOME:-}" = /.local/share ] && unset XDG_DATA_HOME
    [ "${XDG_STATE_HOME:-}" = /.local/state ] && unset XDG_STATE_HOME
fi

HOME="${HOME:-/}"
home_base="${HOME%/}"
export HOME
export XDG_CONFIG_HOME="${XDG_CONFIG_HOME:-${home_base}/.config}"
export XDG_CACHE_HOME="${XDG_CACHE_HOME:-${home_base}/.cache}"
export XDG_DATA_HOME="${XDG_DATA_HOME:-${home_base}/.local/share}"
export XDG_STATE_HOME="${XDG_STATE_HOME:-${home_base}/.local/state}"
unset home_base

mkdir -p "$XDG_CONFIG_HOME" "$XDG_CACHE_HOME" "$XDG_DATA_HOME" "$XDG_STATE_HOME" 2>/dev/null || true

if [ -z "${XDG_RUNTIME_DIR:-}" ] || [ ! -d "$XDG_RUNTIME_DIR" ]; then
    if [ -d "/run/user/$USER_ID" ]; then
        XDG_RUNTIME_DIR="/run/user/$USER_ID"
    else
        XDG_RUNTIME_DIR="/tmp/runtime-$USER_ID"
    fi
    export XDG_RUNTIME_DIR
fi
mkdir -p "$XDG_RUNTIME_DIR" 2>/dev/null || true
chmod 0700 "$XDG_RUNTIME_DIR" 2>/dev/null || true

if [ "$USER_ID" = 0 ]; then
    # rcS owns the root graph in /run/pipewire. Route both native and Pulse
    # clients there before the Plasma D-Bus session is created.
    export PIPEWIRE_RUNTIME_DIR=/run/pipewire
    export PULSE_RUNTIME_PATH=/run/pipewire/pulse
    # Avoid WebKitGTK's assertion on a nonempty unix: PULSE_SERVER value.
    if [ "${PULSE_SERVER:-}" = unix:/run/pipewire/pulse/native ]; then
        unset PULSE_SERVER
    fi
else
    PIPEWIRE_LOG="$XDG_RUNTIME_DIR/pipewire.log"

    if command -v pipewire >/dev/null 2>&1 &&
       [ ! -S "$XDG_RUNTIME_DIR/pipewire-0" ]; then
        XDG_RUNTIME_DIR="$XDG_RUNTIME_DIR" nohup pipewire >>"$PIPEWIRE_LOG" 2>&1 &
        for i in 1 2 3 4 5 6 7 8 9 10; do
            [ -S "$XDG_RUNTIME_DIR/pipewire-0" ] && break
            sleep 0.5
        done
    fi

    if [ -S "$XDG_RUNTIME_DIR/pipewire-0" ]; then
        if command -v wireplumber >/dev/null 2>&1; then
            # Check the current user's manager; another login may have its own
            # WirePlumber process alongside this session's PipeWire daemon.
            if command -v pgrep >/dev/null 2>&1; then
                if ! pgrep -u "$USER_ID" -x wireplumber >/dev/null 2>&1; then
                    XDG_RUNTIME_DIR="$XDG_RUNTIME_DIR" nohup wireplumber >>"$PIPEWIRE_LOG" 2>&1 &
                fi
            elif ! pidof wireplumber >/dev/null 2>&1; then
                XDG_RUNTIME_DIR="$XDG_RUNTIME_DIR" nohup wireplumber >>"$PIPEWIRE_LOG" 2>&1 &
            fi
        fi

        if command -v pipewire-pulse >/dev/null 2>&1; then
            mkdir -p "$XDG_RUNTIME_DIR/pulse" 2>/dev/null || true
            chmod 0700 "$XDG_RUNTIME_DIR/pulse" 2>/dev/null || true
            if [ ! -S "$XDG_RUNTIME_DIR/pulse/native" ]; then
                XDG_RUNTIME_DIR="$XDG_RUNTIME_DIR" nohup pipewire-pulse >>"$PIPEWIRE_LOG" 2>&1 &
                for i in 1 2 3 4 5 6 7 8 9 10; do
                    [ -S "$XDG_RUNTIME_DIR/pulse/native" ] && break
                    sleep 0.5
                done
            fi
        fi
    fi
fi

exec /usr/lib/plasma-dbus-run-session-if-needed \
    /usr/bin/startplasma-wayland "$@"
