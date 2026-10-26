#!/bin/sh
# Plasma's Wayland desktop entry bypasses the startplasma-x11 wrapper. Set up
# the same PipeWire routing before starting D-Bus-activated Plasma services.

USER_ID="$(id -u 2>/dev/null || echo 0)"
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
