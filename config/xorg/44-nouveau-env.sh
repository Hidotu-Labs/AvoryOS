if [ -r "${AVORY_XORG_CONFIG:-/etc/X11/xorg.conf.d/10-modesetting.conf}" ] &&
    grep -q 'Identifier "NouveauPrimary"' "${AVORY_XORG_CONFIG:-/etc/X11/xorg.conf.d/10-modesetting.conf}"; then
    # Only force Zink/NVK on Turing (0x1e00+) and newer; pre-Turing uses Gallium nouveau.
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
        : "${NOUVEAU_USE_ZINK:=1}"
        : "${LIBGL_KOPPER_DRI2:=true}"
        export NOUVEAU_USE_ZINK LIBGL_KOPPER_DRI2
    fi
    unset _nouveau_supports_nvk _drm_dev _vendor _dev_id
fi
