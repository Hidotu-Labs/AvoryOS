#!/bin/sh
# startw.sh — start Weston (Wayland compositor) for AscentOS
#
# Session launched inside XWayland is selected by ASCENT_SESSION (default: none):
#   none    — just Weston with its built-in panel (default)
#   xfce4   — auto-launch XFCE4 inside XWayland once Weston is ready
#
# Renderer is selected by ASCENT_RENDERER (default: auto):
#   auto    — GPU (radeonsi) when the session runs on amdgpu, else pixman
#   gpu     — force the GPU gl-renderer (radeonsi)
#   llvmpipe / gl — Mesa llvmpipe software OpenGL path
#   pixman  — CPU software rasteriser, safe on all QEMU configs
#
# Examples:
#   /bin/startw.sh
#   ASCENT_SESSION=xfce4 /bin/startw.sh
#   ASCENT_RENDERER=llvmpipe /bin/startw.sh

uid=$(id -u)

# ── XDG_RUNTIME_DIR ──────────────────────────────────────────────────────
: "${XDG_RUNTIME_DIR:=/tmp/ascent-runtime-$uid}"
export XDG_RUNTIME_DIR
mkdir -p "$XDG_RUNTIME_DIR" || exit 1
chmod 0700 "$XDG_RUNTIME_DIR" || exit 1
rm -f "$XDG_RUNTIME_DIR"/wayland-0*

# ── seatd ────────────────────────────────────────────────────────────────
SEATD_PID=
if [ -S /run/seatd.sock ]; then
    export SEATD_SOCK="/run/seatd.sock"
elif command -v seatd >/dev/null 2>&1 && [ "$uid" -eq 0 ]; then
    export SEATD_SOCK="/run/seatd.sock"
    rm -f "$SEATD_SOCK"
    seatd -u "${USER:-root}" &
    SEATD_PID=$!
    sleep 1
else
    echo "[startw] /run/seatd.sock missing; start the AscentD seatd service first" >&2
    exit 1
fi

cleanup() {
    [ -z "$SEATD_PID" ] || kill "$SEATD_PID" 2>/dev/null || true
}
trap cleanup EXIT HUP INT TERM

# ── Cursor / theme ────────────────────────────────────────────────────────
export XCURSOR_THEME=Adwaita
export XCURSOR_SIZE=24
export XCURSOR_PATH=/usr/share/icons/

# ── Debug logging ─────────────────────────────────────────────────────────
if [ "${ASCENT_GRAPHICS_DEBUG:-0}" = "1" ]; then
    export WAYLAND_DEBUG=1
    export WESTON_DEBUG_COMPOSITOR=1
    export WLR_LOG_LEVEL=debug
else
    unset WAYLAND_DEBUG WESTON_DEBUG_COMPOSITOR WLR_LOG_LEVEL
fi

# ── DRM card + renderer selection (Phase 6 C7) ───────────────────────────
# Prefer amdgpu when it can drive a display (a monitor, or the C6/C7 emulated
# sink kept by kpi_emu_sink=1), else the native card0.  On amdgpu the default
# renderer is the real GPU (radeonsi via Weston's gl-renderer); card0 keeps
# the pixman software default because ascentdrm has no render node.
DRM_CARD="$(/bin/drm-pick.sh 2>/dev/null)"
[ -n "$DRM_CARD" ] || DRM_CARD=/dev/dri/card0
CARD_NAME="${DRM_CARD#/dev/dri/}"
echo "[startw] DRM card: $DRM_CARD"

# Alpine installs Mesa's DRI drivers under /usr/lib/xorg/modules/dri, while
# Mesa's default search path is /usr/lib/dri: without LIBGL_DRIVERS_PATH the
# EGL/GBM loader finds no radeonsi and silently falls back to llvmpipe.
if [ -d /usr/lib/xorg/modules/dri ]; then
    export LIBGL_DRIVERS_PATH="/usr/lib/xorg/modules/dri${LIBGL_DRIVERS_PATH:+:$LIBGL_DRIVERS_PATH}"
fi

export WLR_RENDERER_ALLOW_SOFTWARE=1

renderer=pixman
case "${ASCENT_RENDERER:-auto}" in
    auto)
        if [ "$DRM_CARD" != "/dev/dri/card0" ]; then
            renderer=gl
            unset GBM_ALWAYS_SOFTWARE LIBGL_ALWAYS_SOFTWARE GALLIUM_DRIVER
        else
            renderer=pixman
            unset GBM_ALWAYS_SOFTWARE
            export LIBGL_ALWAYS_SOFTWARE=1
            export GALLIUM_DRIVER=llvmpipe
        fi
        ;;
    gpu|radeonsi)
        renderer=gl
        FORCE_GL=1
        unset GBM_ALWAYS_SOFTWARE LIBGL_ALWAYS_SOFTWARE GALLIUM_DRIVER
        ;;
    llvmpipe|gl)
        renderer=gl
        FORCE_GL=1
        export GBM_ALWAYS_SOFTWARE=1
        export LIBGL_ALWAYS_SOFTWARE=1
        export GALLIUM_DRIVER=llvmpipe
        ;;
    pixman)
        renderer=pixman
        unset GBM_ALWAYS_SOFTWARE
        export LIBGL_ALWAYS_SOFTWARE=1
        export GALLIUM_DRIVER=llvmpipe
        ;;
    *)
        echo "[startw] Unknown ASCENT_RENDERER='${ASCENT_RENDERER}' (expected: auto, gpu, llvmpipe, pixman)" >&2
        exit 2
        ;;
esac

run_weston() {
    weston \
        --backend=drm-backend.so \
        --renderer="$1" \
        --drm-device="$CARD_NAME" \
        -c /etc/weston.ini \
        --log="$LOG"
}

# ── C7 helper diagnostics ────────────────────────────────────────────────
# Weston spawns /usr/libexec/weston-desktop-shell and weston-keyboard itself
# and only reports "apparently cannot run at all" when they die immediately;
# their own stderr is lost.  Wrap them once so their output and exit status
# land in /tmp/helper-<name>.log, which is dumped below if Weston fails.
install_helper_probe() {
    helper="/usr/libexec/$1"
    [ -x "$helper" ] || return 0
    [ -e "${helper}.real" ] && return 0

    mv "$helper" "${helper}.real"
    cat > "$helper" <<'PROBE'
#!/bin/sh
helper="$0"
log="/tmp/helper-$(basename "$helper").log"
{
    echo "=== $(basename "$helper") started $(date) uid=$(id -u) XDG_RUNTIME_DIR=$XDG_RUNTIME_DIR WAYLAND_DISPLAY=$WAYLAND_DISPLAY"
    "${helper}.real" "$@"
    st=$?
    echo "=== $(basename "$helper") exited status=$st"
} >>"$log" 2>&1
exit $st
PROBE
    chmod +x "$helper"
}
install_helper_probe weston-desktop-shell
install_helper_probe weston-keyboard

LOG="/tmp/weston-$uid.log"
echo "[startw] starting weston at $(date) (renderer: ${renderer})" > "$LOG"
echo "[startw] renderer: ${renderer}"

# ── XFCE4 auto-launch inside XWayland ────────────────────────────────────
# When ASCENT_SESSION=xfce4 we wait for Weston to export its XWayland
# display socket, then launch start-xfce4-wayland in the background.
launch_xfce4_when_ready() {
    echo "[startw] waiting for XWayland display socket..." >> "$LOG"
    for i in $(seq 1 30); do
        for d in 10 0 1 2 3 4 5; do
            if [ -S "/tmp/.X11-unix/X${d}" ]; then
                echo "[startw] XWayland found on :${d}, launching XFCE4..." >> "$LOG"
                echo "[startw] XWayland ready on :${d} — starting XFCE4"
                DISPLAY=":${d}" \
                XDG_SESSION_TYPE=x11 \
                XDG_CURRENT_DESKTOP=XFCE \
                HOME="${HOME:-/}" \
                    /usr/bin/start-xfce4-wayland >> "$LOG" 2>&1 &
                return 0
            fi
        done
        sleep 1
    done
    echo "[startw] Warning: XWayland socket not found after 30s" >> "$LOG"
    echo "[startw] Warning: XWayland socket not found — XFCE4 not launched"
}

case "${ASCENT_SESSION:-none}" in
    xfce4)
        launch_xfce4_when_ready &
        ;;
    none|*)
        # nothing — user can open weston-terminal and run start-xfce4-wayland manually
        ;;
esac

# ── Start Weston ──────────────────────────────────────────────────────────
# Run Weston in the background first so a Wayland client can be used to
# prove the compositor is actually serving its socket (there may be no
# physical monitor on the passed GPU to look at), then wait on it as before.
run_weston "$renderer" &
WESTON_PID=$!

WL_SOCKET=""
for i in $(seq 1 40); do
    for s in "$XDG_RUNTIME_DIR"/wayland-*; do
        case "$s" in
            *.lock) continue ;;
        esac
        if [ -S "$s" ]; then
            WL_SOCKET="$s"
            break
        fi
    done
    [ -n "$WL_SOCKET" ] && break
    sleep 0.5
done

if [ -n "$WL_SOCKET" ]; then
    WAYLAND_DISPLAY="$(basename "$WL_SOCKET")"
    export WAYLAND_DISPLAY
    echo "[startw] wayland socket: $WL_SOCKET"

    if command -v es2gears_wayland >/dev/null 2>&1; then
        sleep 2
        es2gears_wayland >/tmp/wayland-probe.log 2>&1 &
        PROBE_PID=$!
        sleep 3
        if kill -0 "$PROBE_PID" 2>/dev/null; then
            echo "[startw] wayland probe: es2gears_wayland is rendering (compositor + GL OK)"
            kill "$PROBE_PID" 2>/dev/null
            wait "$PROBE_PID" 2>/dev/null
        else
            wait "$PROBE_PID"
            echo "[startw] wayland probe: es2gears_wayland exited $?"
            echo "[startw] --- /tmp/wayland-probe.log ---"
            cat /tmp/wayland-probe.log 2>/dev/null || true
            echo "[startw] --- end /tmp/wayland-probe.log ---"
        fi
    else
        echo "[startw] no es2gears_wayland client available for a probe"
    fi
else
    echo "[startw] warning: no wayland socket appeared" >&2
fi

wait "$WESTON_PID"
status=$?

# If the GPU renderer could not come up (Mesa/radeonsi failure, no render
# node, ...), keep the session usable by falling back to pixman on the same
# card.  An explicit ASCENT_RENDERER=gpu/llvmpipe is taken as final.
if [ "$status" -ne 0 ] && [ "$renderer" = "gl" ] && [ -z "${FORCE_GL:-}" ]; then
    echo "[startw] weston gl-renderer failed (status $status); retrying with pixman"
    echo "[startw] retrying with pixman after gl-renderer failure" >> "$LOG"
    unset GBM_ALWAYS_SOFTWARE
    export LIBGL_ALWAYS_SOFTWARE=1
    export GALLIUM_DRIVER=llvmpipe
    renderer=pixman
    run_weston "$renderer"
    status=$?
fi

if [ "$status" -ne 0 ]; then
    echo "[startw] weston exited with status $status"
    echo "[startw] --- $LOG ---"
    cat "$LOG" 2>/dev/null || true
    echo "[startw] --- end $LOG ---"
    for l in /tmp/helper-*.log; do
        [ -e "$l" ] || continue
        echo "[startw] --- $l ---"
        cat "$l"
        echo "[startw] --- end $l ---"
    done
fi
exit "$status"
