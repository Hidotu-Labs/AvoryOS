#!/usr/bin/env bash
# Keep only the NVIDIA chips named in NOUVEAU_CHIPS (space/comma separated,
# e.g. "ad106" or "tu116 ga104") under FIRMWARE_DIR/nvidia. Empty keeps all.
set -euo pipefail
dir="${1:?usage: prune-nvidia-firmware.sh FIRMWARE_DIR}/nvidia"
chips="${NOUVEAU_CHIPS:-}"
[[ -n "${chips//[ ,]/}" && -d "$dir" ]] || exit 0
read -r -a keep <<< "${chips//,/ }"
for chip in "${keep[@]}"; do
    [[ -d "$dir/$chip" ]] || { echo "NOUVEAU_CHIPS: no firmware for '$chip' in $dir" >&2; exit 1; }
done
for path in "$dir"/*/; do
    chip="$(basename "$path")"
    [[ " ${keep[*]} " == *" $chip "* ]] && continue
    rm -rf "$path"
done
