if [ -r "${AVORY_XORG_CONFIG:-/etc/X11/xorg.conf.d/10-modesetting.conf}" ] &&
    grep -q 'Identifier "NouveauPrimary"' "${AVORY_XORG_CONFIG:-/etc/X11/xorg.conf.d/10-modesetting.conf}"; then
    : "${NOUVEAU_USE_ZINK:=1}"
    : "${LIBGL_KOPPER_DRI2:=true}"
    export NOUVEAU_USE_ZINK LIBGL_KOPPER_DRI2
fi
