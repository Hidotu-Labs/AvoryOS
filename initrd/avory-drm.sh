if [ -z "${KWIN_DRM_DEVICES:-}" ] && [ -x /bin/drm-pick.sh ]; then
    _avory_card="$(/bin/drm-pick.sh 2>/dev/null || true)"
    case "$_avory_card" in
        "") ;;
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
