# /etc/profile.d/avory-drm.sh — prefer amdgpu for GPU-accelerated sessions.
#
# KWin (Wayland) selects its DRM device from KWIN_DRM_DEVICES; Xorg reads the
# kmsdev option that startx.sh writes, and startw.sh passes --drm-device to
# Weston.  This covers the Plasma Wayland session started from a login shell:
# when /bin/drm-pick.sh has an amdgpu card that can drive a display (monitor
# or the C7 emulated sink), KWin renders and outputs on it; otherwise the
# variable stays unset and KWin keeps the native card0.
if [ -z "${KWIN_DRM_DEVICES:-}" ] && [ -x /bin/drm-pick.sh ]; then
    _avory_card="$(/bin/drm-pick.sh 2>/dev/null)"
    case "$_avory_card" in
        ""|/dev/dri/card0) ;;
        *) KWIN_DRM_DEVICES="$_avory_card"; export KWIN_DRM_DEVICES ;;
    esac
    unset _avory_card
fi

# Mesa's DRI drivers live under /usr/lib/xorg/modules/dri on Alpine; EGL/GBM
# clients (and Weston's gl-renderer) need the path or they fall back to
# llvmpipe.  Keep an existing path if something already set one.
if [ -d /usr/lib/xorg/modules/dri ]; then
    case ":${LIBGL_DRIVERS_PATH:-}:" in
        *:/usr/lib/xorg/modules/dri:*) ;;
        *)
            LIBGL_DRIVERS_PATH="/usr/lib/xorg/modules/dri${LIBGL_DRIVERS_PATH:+:$LIBGL_DRIVERS_PATH}"
            export LIBGL_DRIVERS_PATH
            ;;
    esac
fi
