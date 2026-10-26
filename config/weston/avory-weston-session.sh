#!/bin/sh
# Weston does not consume KWin/wlroots device variables. Select its KMS card.
for option in "$@"; do
    case "$option" in
        --drm-device|--drm-device=*|--backend|--backend=*|-B|-B*)
            exec /usr/bin/weston "$@" ;;
    esac
done
if [ -x /bin/drm-pick.sh ]; then
    display=$(/bin/drm-pick.sh --display) || exit 1
    [ -n "$display" ] || exit 1
    exec /usr/bin/weston "--drm-device=${display##*/}" "$@"
fi
exec /usr/bin/weston "$@"
