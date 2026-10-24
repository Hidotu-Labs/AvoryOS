if [ -x /bin/drm-pick.sh ]; then
    _avory_session="$(/bin/drm-pick.sh --wayland 2>/dev/null || true)"
    if [ -n "$_avory_session" ]; then
        read -r _avory_display _avory_render _avory_driver _avory_cards <<EOF
$_avory_session
EOF
        if [ -z "${KWIN_DRM_DEVICES:-}" ]; then
            export KWIN_DRM_DEVICES="$_avory_cards"
        fi
        if [ "${KWIN_RENDER_NODES+x}" != x ] && [ "$_avory_render" != - ]; then
            export KWIN_RENDER_NODES="$_avory_render"
        fi
        if [ "$_avory_driver" = nouveau ]; then
            # Current NVIDIA hardware uses Mesa's NVK Vulkan driver through
            # Zink for OpenGL, including KWin's EGL renderer and Qt clients.
            export NOUVEAU_USE_ZINK="${NOUVEAU_USE_ZINK:-1}"
            export MESA_VK_WSI_PRESENT_MODE="${MESA_VK_WSI_PRESENT_MODE:-immediate}"
        fi
    fi
    unset _avory_session _avory_display _avory_render _avory_driver _avory_cards
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
