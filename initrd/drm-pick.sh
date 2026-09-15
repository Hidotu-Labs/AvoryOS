#!/bin/sh
# drm-pick.sh — choose the DRM card for a desktop session (Phase 6 C7).
#
# Prints one line on stdout: /dev/dri/cardN.
#
# amdgpu's card is preferred when it can actually drive a display: either one
# of its connectors reports "connected" (a real monitor, or the EDID-override
# emulated sink the DCN self-test leaves in place), or the kernel was booted
# with kpi_emu_sink=1 (the C7 boot parameter that keeps that emulated sink).
# Otherwise the native card0 (virtio-vga/ascentdrm) stays selected, so the
# visible QEMU desktop keeps working on boots without amdgpu.
#
# ASCENT_DRM_CARD overrides the choice ("card2", "2" or "/dev/dri/card2").

set -u

normalize() {
    case "$1" in
        /dev/dri/*) echo "$1" ;;
        card[0-9]*) echo "/dev/dri/$1" ;;
        [0-9]*)     echo "/dev/dri/card$1" ;;
        *)          echo "" ;;
    esac
}

if [ -n "${ASCENT_DRM_CARD:-}" ]; then
    card="$(normalize "$ASCENT_DRM_CARD")"
    if [ -n "$card" ] && [ -e "$card" ]; then
        echo "$card"
        exit 0
    fi
fi

find_amdgpu_card() {
    for dev in /dev/dri/card*; do
        [ -e "$dev" ] || continue
        name="${dev#/dev/dri/}"
        drv="$(readlink -f "/sys/class/drm/$name/device/driver" 2>/dev/null || true)"
        if [ "${drv##*/}" = "amdgpu" ]; then
            echo "$dev"
            return 0
        fi
    done
    # Sysfs may not expose the driver link through the LinuxKPI device model;
    # the current minor layout puts amdgpu on card2 with renderD129.
    if [ -e /dev/dri/card2 ] && [ -e /dev/dri/renderD129 ]; then
        echo "/dev/dri/card2"
        return 0
    fi
    return 1
}

card_can_display() {
    card="$1"
    name="${card#/dev/dri/}"
    for status in /sys/class/drm/"$name"-*/status; do
        [ -r "$status" ] || continue
        if [ "$(cat "$status" 2>/dev/null)" = "connected" ]; then
            return 0
        fi
    done
    # The DCN self-test kept its emulated sink for this session; trust the
    # boot parameter when sysfs exposes no connector status.
    if grep -q 'kpi_emu_sink=1' /proc/cmdline 2>/dev/null; then
        return 0
    fi
    return 1
}

amd="$(find_amdgpu_card || true)"
if [ -n "$amd" ] && card_can_display "$amd"; then
    echo "$amd"
    exit 0
fi

echo /dev/dri/card0
