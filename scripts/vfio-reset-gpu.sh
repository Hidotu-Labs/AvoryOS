#!/usr/bin/env bash
# vfio-reset-gpu.sh -- reset a passed-through GPU on the host between runs.
#
# Closing/reopening QEMU can exercise VFIO reset and power-management paths.
# A completed reset request is not evidence of a physical cold start. A guest
# killed during PSP/RLC initialization can leave state that the next probe does
# not recover; the failed stage and actual host reset path need evidence.
#
# Raphael exposes bus-only reset and shares a bridge with host-owned functions.
# This helper performs host writes when explicitly invoked. Do not invoke it
# automatically to retry initialization or to investigate the recurring host
# reboot. See docs/amdgpu-testing.md for the current unresolved incident.
#
# Reset must be attempted while the device is still bound to vfio-pci.
# Unbinding first can put the function into D3cold, after which it no longer
# answers config space and every reset method returns -ENOTTY (this is what
# the old unbind -> reset -> rebind recipe in docs/amdgpu-testing.md hit).
#
# Usage: sudo scripts/vfio-reset-gpu.sh [BDF]
#   BDF   PCI address of the passed-through GPU (default: 0000:0e:00.0)
#
# Exit status: 0 when the device was reset and is bound to vfio-pci; 1 when
# no reset method is available (a host reboot is then required).
set -euo pipefail

BDF="${1:-0000:0e:00.0}"
DEV="/sys/bus/pci/devices/$BDF"
RESET="$DEV/reset"
RESET_METHOD="$DEV/reset_method"
VFIO_BIND="/sys/bus/pci/drivers/vfio-pci/bind"

if [ "$(id -u)" != 0 ]; then
    echo "vfio-reset-gpu: must run as root" >&2
    exit 1
fi

if [ ! -e "$DEV" ]; then
    echo "vfio-reset-gpu: no such device: $DEV" >&2
    echo "vfio-reset-gpu: check the BDF (lspci -D | grep -i vga)" >&2
    exit 1
fi

driver="unbound"
if [ -e "$DEV/driver" ]; then
    driver="$(basename "$(readlink -f "$DEV/driver")")"
fi

if [ "$driver" != "unbound" ] && [ "$driver" != "vfio-pci" ]; then
    echo "vfio-reset-gpu: $BDF is bound to '$driver', not vfio-pci; refusing" >&2
    exit 1
fi

# Rebind helper used on every exit path so the device is never left orphaned.
# An orphaned function has no /dev/vfio/<group> and the next run-vfio fails
# with "Could not open /dev/vfio/N: No such file or directory".
rebind_vfio() {
    [ -e "$DEV/driver" ] && return 0
    [ -e "$VFIO_BIND" ] || return 0

    echo "vfio-reset-gpu: binding $BDF to vfio-pci"
    if ! echo "$BDF" > "$VFIO_BIND" 2>/dev/null; then
        # No ID match and no override (this box binds vfio-pci out of band):
        # force the match, then bind.
        echo vfio-pci > "$DEV/driver_override" 2>/dev/null || true
        echo "$BDF" > "$VFIO_BIND" 2>/dev/null || true
    fi
    return 0
}
trap rebind_vfio EXIT

# A reset is only usable while the function is in D0 and bound; bind it first
# if a previous failed attempt left it orphaned.
rebind_vfio
if [ -e "$DEV/driver" ]; then
    driver="$(basename "$(readlink -f "$DEV/driver")")"
fi

if [ ! -e "$RESET" ]; then
    echo "vfio-reset-gpu: $BDF has no reset attribute" >&2
    exit 1
fi

methods="$(cat "$RESET_METHOD" 2>/dev/null || true)"
echo "vfio-reset-gpu: $BDF reset methods: ${methods:-<none>}"

if [ -z "$methods" ]; then
    echo "vfio-reset-gpu: the host kernel reports no reset method for $BDF;" \
         "a host reboot is required to cold-start the GPU" >&2
    exit 1
fi

echo "vfio-reset-gpu: resetting $BDF (while bound to ${driver})"
if ! echo 1 > "$RESET"; then
    echo "vfio-reset-gpu: reset failed; a host reboot is required" >&2
    exit 1
fi

# The reset may have re-enumerated the function; make sure it is bound.
sleep 1
rebind_vfio

if [ ! -e "$DEV/driver" ]; then
    echo "vfio-reset-gpu: $BDF did not come back; check dmesg" >&2
    exit 1
fi

echo "vfio-reset-gpu: $BDF reset request completed and is bound to vfio-pci; cold state is not verified"
