if [ -x /bin/drm-pick.sh ]; then
    _avory_session="$(/bin/drm-pick.sh --wayland 2>/dev/null || true)"
    if [ -n "$_avory_session" ]; then
        read -r _avory_display _avory_render _avory_driver _avory_cards <<EOF
$_avory_session
EOF
        if [ -z "${KWIN_DRM_DEVICES:-}" ]; then
            export KWIN_DRM_DEVICES="$_avory_cards"
        fi
        if [ -z "${WLR_DRM_DEVICES:-}" ]; then
            export WLR_DRM_DEVICES="$_avory_cards"
        fi
        if [ "${KWIN_RENDER_NODES+x}" != x ] && [ "$_avory_render" != - ]; then
            export KWIN_RENDER_NODES="$_avory_render"
        fi
        if [ "$_avory_driver" = nouveau ]; then
            # NVK (and Zink over NVK) only supports Turing (0x1e00+) and newer.
            # Pre-Turing GPUs (Kepler/Maxwell/Pascal) fall back to Gallium nouveau.
            _nouveau_supports_nvk=0
            for _drm_dev in /sys/class/drm/card*/device; do
                [ -r "$_drm_dev/vendor" ] && [ -r "$_drm_dev/device" ] || continue
                read -r _vendor < "$_drm_dev/vendor" || continue
                read -r _dev_id < "$_drm_dev/device" || continue
                if [ "$_vendor" = 0x10de ] && [ "$((_dev_id))" -ge "$((0x1e00))" ]; then
                    _nouveau_supports_nvk=1
                    break
                fi
            done
            if [ "$_nouveau_supports_nvk" -eq 1 ]; then
                export NOUVEAU_USE_ZINK="${NOUVEAU_USE_ZINK:-1}"
                export MESA_VK_WSI_PRESENT_MODE="${MESA_VK_WSI_PRESENT_MODE:-immediate}"
            fi
            unset _nouveau_supports_nvk _drm_dev _vendor _dev_id
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
