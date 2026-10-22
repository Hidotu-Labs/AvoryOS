#!/bin/sh
# avory-xorg-profile — select the Xorg profile matching this boot's GPU topology.
#
# A plain `make run` boot has no VFIO GPU, so an image baked for VFIO leaves
# Xorg pointing at a nonexistent /dev/dri/card2 (BusID PCI:0:3:0) and the
# server dies instantly ("Failed to read display number from pipe"). This
# script copies the matching profile into place so both bare `sddm` and
# the desktop session start on the right device:
#   amdgpu — AMD-primary PRIME layout (passed-through AMD VGA present)
#   nouveau — identity-selected NVK/Zink renderer with native PRIME output
#   native — connected native software-rendered display fallback
#
# Usage: avory-xorg-profile [sysfs-root] [dest] [alternates-dir]
# Defaults: /sys, /etc/X11/xorg.conf.d/10-modesetting.conf, /usr/share/avory-xorg
# AVORY_XORG_PROFILE=amdgpu|nouveau|native overrides detection.
# Prints "xorg-profile: <name>" and exits 0 always; never fail the boot.

set -u

SYS="${1:-/sys}"
DEST="${2:-/etc/X11/xorg.conf.d/10-modesetting.conf}"
ALT="${3:-/usr/share/avory-xorg}"

has_amd_vga() {
    [ -d "$SYS/bus/pci/devices" ] || return 1
    for d in "$SYS"/bus/pci/devices/*; do
        [ -r "$d/vendor" ] && [ -r "$d/class" ] || continue
        [ "$(cat "$d/vendor" 2>/dev/null)" = "0x1002" ] || continue
        driver="$(readlink -f "$d/driver" 2>/dev/null || true)"
        case "$(cat "$d/class" 2>/dev/null):${driver##*/}" in
            0x03*:amdgpu) return 0 ;;
        esac
    done
    return 1
}

PROFILE="native"
if has_amd_vga; then
    PROFILE="amdgpu"
fi
case "${AVORY_XORG_PROFILE:-}" in
    amdgpu|native|nouveau) PROFILE="$AVORY_XORG_PROFILE" ;;
esac

SRC="$ALT/10-modesetting-$PROFILE.conf"
nouveau_selector=${AVORY_NOUVEAU_XORG:-/usr/bin/avory-nouveau-xorg}
nvk_icd=${AVORY_NVK_ICD:-/usr/share/vulkan/icd.d/nouveau_icd.json}
if [ "$PROFILE" = nouveau ] || { [ "$PROFILE" = native ] && [ -z "${AVORY_XORG_PROFILE:-}" ]; }; then
    if [ -x "$nouveau_selector" ] && [ -r "$nvk_icd" ]; then
        prepared="$(mktemp)" || exit 0
        trap 'rm -f "$prepared"' EXIT HUP INT TERM
        if "$nouveau_selector" /bin/drm-identify "$ALT/10-modesetting-nouveau.conf" > "$prepared"; then
            PROFILE=nouveau
            SRC="$prepared"
        else
            rm -f "$prepared"
            if [ "$PROFILE" = nouveau ]; then
                PROFILE=native
                SRC="$ALT/10-modesetting-native.conf"
            fi
        fi
    elif [ "$PROFILE" = nouveau ]; then
        PROFILE=native
        SRC="$ALT/10-modesetting-native.conf"
    fi
fi
if [ ! -f "$SRC" ]; then
    echo "xorg-profile: $PROFILE requested but $SRC missing; keeping $DEST" >&2
    echo "xorg-profile: $PROFILE (unchanged)"
    exit 0
fi
if [ "$PROFILE" = native ] && [ -x /bin/drm-pick.sh ]; then
    selected="$(/bin/drm-pick.sh --display 2>/dev/null || true)"
    case "$selected" in
        /dev/dri/card[0-9]*)
            prepared="$(mktemp)" || exit 0
            trap 'rm -f "$prepared"' EXIT HUP INT TERM
            if sed "s|Option \"kmsdev\" \"/dev/dri/card[0-9]*\"|Option \"kmsdev\" \"$selected\"|" "$SRC" > "$prepared"; then
                SRC="$prepared"
            fi
            ;;
    esac
fi
if [ -f "$DEST" ] && command -v cmp >/dev/null 2>&1 && cmp -s "$SRC" "$DEST" 2>/dev/null; then
    echo "xorg-profile: $PROFILE (already installed)"
    exit 0
fi
DESTDIR="${DEST%/*}"
[ -n "$DESTDIR" ] && mkdir -p "$DESTDIR" 2>/dev/null
if cp -f "$SRC" "$DEST" 2>/dev/null; then
    echo "xorg-profile: $PROFILE"
else
    echo "xorg-profile: $PROFILE requested but cannot write $DEST" >&2
    echo "xorg-profile: $PROFILE (not installed)"
fi
exit 0
