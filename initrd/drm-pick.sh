#!/bin/sh
set -u
mode="${1:---display}"
case "$mode" in --display|--render|--pair|--wayland) ;; *) exit 2 ;; esac
inventory="$(/bin/drm-identify 2>/dev/null)" || exit 1
[ -n "$inventory" ] || exit 1

normalize_card() {
    case "$1" in
        /dev/dri/card*) printf '%s\n' "$1" ;;
        card*) printf '/dev/dri/%s\n' "$1" ;;
        *) printf '/dev/dri/card%s\n' "$1" ;;
    esac
}
display=""
render=""
if [ -n "${ASCENT_DRM_CARD:-}" ]; then
    requested="$(normalize_card "$ASCENT_DRM_CARD")"
    display="$(printf '%s\n' "$inventory" | awk -v requested="$requested" '$1==requested {print $1; exit}')"
    [ -n "$display" ] || echo "drm-pick: requested card is unavailable; using fallback" >&2
fi
if [ -z "$display" ] && [ -n "${ASCENT_DRM_BDF:-}${ASCENT_DRM_DRIVER:-}" ]; then
    bdf="${ASCENT_DRM_BDF:-}"
    case "$bdf" in ""|pci:*) ;; *) bdf="pci:$bdf" ;; esac
    display="$(printf '%s\n' "$inventory" | awk -v bdf="$bdf" -v driver="${ASCENT_DRM_DRIVER:-}" \
        '(!length(bdf)||$3==bdf)&&(!length(driver)||$2==driver) {print $1; exit}')"
    [ -n "$display" ] || echo "drm-pick: requested identity is unavailable; using fallback" >&2
fi
if [ -z "$display" ]; then
    emulated=0
    if grep -qw 'kpi_emu_sink=1' /proc/cmdline 2>/dev/null; then emulated=1; fi
    for driver in amdgpu nouveau ascentdrm; do
        display="$(printf '%s\n' "$inventory" | awk -v driver="$driver" -v emulated="$emulated" \
            '$2==driver&&($4==1||(driver=="amdgpu"&&emulated==1)) {print $1; exit}')"
        [ -z "$display" ] || break
    done
fi
if [ -z "$display" ]; then
    display="$(printf '%s\n' "$inventory" | awk '$2=="ascentdrm"||($4==1&&$2!="vkms") {print $1; exit}')"
fi
if [ -n "${ASCENT_DRM_RENDER:-}" ]; then
    render="$(printf '%s\n' "$inventory" | awk -v requested="$ASCENT_DRM_RENDER" '$5==requested&&$5!="-" {print $5; exit}')"
    [ -n "$render" ] || echo "drm-pick: requested render node is unavailable; using fallback" >&2
fi
if [ -z "$render" ] && [ -n "$display" ]; then
    render="$(printf '%s\n' "$inventory" | awk -v display="$display" '$1==display&&$5!="-"&&$2!="vkms" {print $5; exit}')"
fi
if [ -z "$render" ]; then
    for driver in nouveau amdgpu; do
        render="$(printf '%s\n' "$inventory" | awk -v driver="$driver" '$2==driver&&$5!="-" {print $5; exit}')"
        [ -z "$render" ] || break
    done
fi
case "$mode" in
    --wayland)
        [ -n "$display" ] || exit 1
        # KWin needs primary/card nodes for KMS and render nodes for Mesa.
        # Prefer the selected hardware renderer, retaining the visible sink
        # and other production GPUs. VKMS/VGEM are test devices, not monitors.
        printf '%s\n' "$inventory" | awk -v display="$display" -v render="$render" '
            function add(node) {
                if (length(node) && !seen[node]++)
                    devices = devices (length(devices) ? ":" : "") node
            }
            { cards[NR]=$1; drivers[NR]=$2 }
            $5==render && render!="" { render_card=$1; render_driver=$2 }
            END {
                if (render_driver=="nouveau" || render_driver=="amdgpu")
                    add(render_card)
                add(display)
                for (i=1; i<=NR; i++)
                    if (drivers[i]=="nouveau" || drivers[i]=="amdgpu" || drivers[i]=="ascentdrm")
                        add(cards[i])
                printf "%s %s %s %s\n", display, length(render) ? render : "-",
                    length(render_driver) ? render_driver : "-", devices
            }'
        ;;
    --display) [ -n "$display" ] && printf '%s\n' "$display" ;;
    --render) [ -n "$render" ] && printf '%s\n' "$render" ;;
    --pair) [ -n "$display" ] && printf '%s %s\n' "$display" "${render:--}" ;;
esac
