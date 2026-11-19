#!/bin/sh
# Sourced before Plasma starts. Bring up the audio graph independently so
# a slow user manager or PipeWire socket cannot hold up the desktop.
(
    if [ -n "${XDG_RUNTIME_DIR:-}" ] && command -v pipewire >/dev/null 2>&1; then
        if [ -S "$XDG_RUNTIME_DIR/systemd/private" ] &&
           command -v systemctl >/dev/null 2>&1; then
            if timeout 3s systemctl --user --no-block start pipewire.service wireplumber.service \
                pipewire-pulse.service; then
                exit 0
            fi
        fi

        if [ ! -S "$XDG_RUNTIME_DIR/pipewire-0" ]; then
            XDG_RUNTIME_DIR="$XDG_RUNTIME_DIR" nohup pipewire \
                >>"$XDG_RUNTIME_DIR/pipewire.log" 2>&1 &
            for _avory_pw_wait in 1 2 3 4 5 6 7 8 9 10; do
                [ -S "$XDG_RUNTIME_DIR/pipewire-0" ] && break
                sleep 0.2
            done
        fi

        if [ -S "$XDG_RUNTIME_DIR/pipewire-0" ]; then
            _avory_wp_running=0
            if command -v pgrep >/dev/null 2>&1; then
                for _avory_wp_pid in $(pgrep -u "$(id -u)" -x wireplumber 2>/dev/null || true); do
                    if [ -r "/proc/$_avory_wp_pid/environ" ] &&
                       tr '\0' '\n' <"/proc/$_avory_wp_pid/environ" 2>/dev/null |
                           grep -Fx "XDG_RUNTIME_DIR=$XDG_RUNTIME_DIR" >/dev/null; then
                        _avory_wp_running=1
                        break
                    fi
                done
            elif command -v pidof >/dev/null 2>&1 &&
                 pidof wireplumber >/dev/null 2>&1; then
                _avory_wp_running=1
            fi
            if command -v wireplumber >/dev/null 2>&1 &&
               [ "$_avory_wp_running" = 0 ]; then
                XDG_RUNTIME_DIR="$XDG_RUNTIME_DIR" nohup wireplumber \
                    >>"$XDG_RUNTIME_DIR/pipewire.log" 2>&1 &
            fi
            if command -v pipewire-pulse >/dev/null 2>&1 &&
               [ ! -S "$XDG_RUNTIME_DIR/pulse/native" ]; then
                mkdir -p "$XDG_RUNTIME_DIR/pulse" 2>/dev/null || true
                XDG_RUNTIME_DIR="$XDG_RUNTIME_DIR" nohup pipewire-pulse \
                    >>"$XDG_RUNTIME_DIR/pipewire.log" 2>&1 &
            fi
        fi
    fi
) </dev/null >/dev/null 2>&1 &
