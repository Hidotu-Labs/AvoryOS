#!/bin/sh
set -eu
identify=${1:-/bin/drm-identify}
template=${2:-/usr/share/avory-xorg/10-modesetting-nouveau.conf}
inventory=$("$identify") || exit 1
selection=$(printf '%s\n' "$inventory" | awk '
    $2=="nouveau" && $3 ~ /^pci:/ && $5!="-" {render=$1; renderbus=$3; renders++}
    $2=="ascentdrm" && $3 ~ /^pci:/ && $4==1 {display=$1; displaybus=$3; displays++}
    END {if(renders==1 && displays==1) print render,renderbus,display,displaybus; else exit 1}') || exit 1
set -- $selection
[ "$#" -eq 4 ] || exit 1
busid() {
    address=${1#pci:}
    oldifs=$IFS
    IFS=:.
    set -- $address
    IFS=$oldifs
    [ "$#" -eq 4 ] && [ "$1" = 0000 ] || return 1
    case "$2$3$4" in *[!0-9a-fA-F]*) return 1 ;; esac
    printf 'PCI:%d:%d:%d' "$((0x$2))" "$((0x$3))" "$((0x$4))"
}
renderbus=$(busid "$2") || exit 1
displaybus=$(busid "$4") || exit 1
case "$1:$3" in /dev/dri/card[0-9]*:/dev/dri/card[0-9]*) ;; *) exit 1 ;; esac
sed -e "s|@RENDER_CARD@|$1|g" -e "s|@RENDER_BUS@|$renderbus|g" \
    -e "s|@DISPLAY_CARD@|$3|g" -e "s|@DISPLAY_BUS@|$displaybus|g" "$template"
