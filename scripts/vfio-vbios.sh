#!/usr/bin/env bash
# vfio-vbios.sh -- extract a GPU's VBIOS image on the host for QEMU passthrough.
#
# A passthrough guest does not receive the host's ACPI VFCT table, so the
# guest kernel (amdgpu, later) reads the VBIOS from the device's ROM BAR and
# QEMU loads that BAR from the file this script produces (romfile=).
#
# Two sources are supported:
#   1. the device's ROM BAR via /sys/bus/pci/devices/<BDF>/rom (discrete
#      cards with an option ROM; requires the device to be bound to vfio-pci
#      so the read does not race its driver);
#   2. the system firmware's VFCT ACPI table when the ROM node does not exist
#      (APUs/iGPUs such as Raphael keep their VBIOS in system firmware; the
#      table entry matching the BDF's bus/device/function is extracted).
#
# VFCT images are padded to the next power of two with 0xFF, matching a real
# option-ROM image and the ROM BAR size QEMU exposes, so the guest's
# pci_map_rom() size and CRC32 compare cleanly with this file.
#
# Reading the ROM node or VFCT requires root.
#
# Usage: scripts/vfio-vbios.sh [BDF] [OUTPUT]
#   BDF      PCI address of the passed-through GPU (default: 0000:0e:00.0)
#   OUTPUT   destination image (default: build/vfio/vbios.rom)
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BDF="${1:-0000:0e:00.0}"
OUT="${2:-$ROOT/build/vfio/vbios.rom}"
ROM="/sys/bus/pci/devices/$BDF/rom"
VFCT="/sys/firmware/acpi/tables/VFCT"

if [ "$(id -u)" != 0 ]; then
    echo "vfio-vbios: must run as root to read the ROM node or VFCT" >&2
    exit 1
fi

if [ ! -e "/sys/bus/pci/devices/$BDF" ]; then
    echo "vfio-vbios: no such device: /sys/bus/pci/devices/$BDF" >&2
    echo "vfio-vbios: check the BDF (lspci -D | grep -i vga)" >&2
    exit 1
fi

mkdir -p "$(dirname "$OUT")"

driver_path="$(readlink -f "/sys/bus/pci/devices/$BDF/driver" 2>/dev/null || true)"
if [ -n "$driver_path" ] && [ "$(basename "$driver_path")" != "vfio-pci" ]; then
    echo "vfio-vbios: warning: device is bound to $(basename "$driver_path");"
    echo "vfio-vbios: ROM BAR reads may race the driver (bind vfio-pci first)."
fi

# ── source 1: the device's ROM BAR ───────────────────────────────────────────

if [ -e "$ROM" ]; then
    echo "vfio-vbios: reading ROM BAR of $BDF"
    # Enable the ROM BAR, read the image, disable it again.
    if echo 1 > "$ROM" 2>/dev/null && cat "$ROM" > "$OUT" 2>/dev/null && [ -s "$OUT" ]; then
        echo 0 > "$ROM" 2>/dev/null || true
        echo "vfio-vbios: wrote $OUT ($(stat -c %s "$OUT") bytes) from the ROM BAR"
        exit 0
    fi
    echo 0 > "$ROM" 2>/dev/null || true
    rm -f "$OUT"
    echo "vfio-vbios: ROM BAR read returned nothing; falling back to VFCT" >&2
else
    echo "vfio-vbios: no ROM node for $BDF (APU/iGPU); falling back to VFCT"
fi

# ── source 2: the VFCT ACPI table ────────────────────────────────────────────
#
# Layout (Linux 6.6 atomfirmware.h):
#   uefi_acpi_vfct:  ACPI header 0x00..0x23, table_uuid[16] at 0x24,
#                    vbiosimageoffset at 0x34
#   vfct_image_header (0x1C bytes): pcibus u32, pcidevice u32, pcifunction u32,
#                    vendorid u16, deviceid u16, ssvid u16, ssid u16,
#                    revision u32, imagelength u32; content follows.
if [ ! -r "$VFCT" ]; then
    echo "vfio-vbios: no ROM node and no readable $VFCT" >&2
    exit 1
fi

python3 - "$VFCT" "$BDF" "$OUT" <<'PY'
import re
import struct
import sys

vfct_path, bdf, out_path = sys.argv[1], sys.argv[2], sys.argv[3]
data = open(vfct_path, "rb").read()

if len(data) < 0x4C or data[:4] != b"VFCT":
    sys.exit("vfio-vbios: not a VFCT table")

vbios_offset = struct.unpack_from("<I", data, 0x34)[0]

m = re.match(r"(?:[0-9a-fA-F]{4}:)?([0-9a-fA-F]{2}):([0-9a-fA-F]{2})\.([0-7])$", bdf)
if not m:
    sys.exit(f"vfio-vbios: cannot parse BDF {bdf!r}")
bus, dev, func = int(m.group(1), 16), int(m.group(2), 16), int(m.group(3))

off = vbios_offset
found = None
while off + 0x1C <= len(data):
    pcibus, pcidev, pcifunc, vid, did, ssvid, ssid, rev, ilen = struct.unpack_from(
        "<IIIHHHHII", data, off
    )
    if ilen == 0 or off + 0x1C + ilen > len(data):
        break
    if pcibus == bus and pcidev == dev and pcifunc == func and vid == 0x1002:
        found = data[off + 0x1C : off + 0x1C + ilen]
        break
    off += 0x1C + ilen

if found is None:
    sys.exit(f"vfio-vbios: no VFCT image for {bdf} (vendor 1002)")

if found[0] != 0x55 or found[1] != 0xAA:
    sys.exit("vfio-vbios: extracted image has no 55AA option-ROM signature")

# Pad to a power of two with 0xFF: real option-ROM images are padded that way
# and the guest sizes the ROM BAR to this exact length.
size = 1
while size < len(found):
    size <<= 1
image = found + b"\xff" * (size - len(found))
open(out_path, "wb").write(image)
print(
    f"vfio-vbios: wrote {out_path} "
    f"({len(found)} bytes image, padded to {size} bytes) from VFCT"
)
PY
