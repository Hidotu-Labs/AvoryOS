#!/usr/bin/env bash
# vfio-check-rom.sh -- compare the guest's logged VBIOS CRC with the host file.
#
# The C7 VFIO self-test logs one line like
#   [INFO] LinuxKPI: vfio rom crc32=0x1234abcd size=65536 first=55 aa
# after pci_map_rom(); this helper parses it and compares both CRC and size
# with build/vfio/vbios.rom (extracted by scripts/vfio-vbios.sh).
#
# Usage: scripts/vfio-check-rom.sh [LOG] [ROM]
#   LOG  serial capture (default: build/logs/p5-vfio.log)
#   ROM  host VBIOS image (default: build/vfio/vbios.rom)
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
LOG="${1:-$ROOT/build/logs/p5-vfio.log}"
ROM="${2:-$ROOT/build/vfio/vbios.rom}"

if [ ! -f "$LOG" ]; then
    echo "vfio-check-rom: no such log: $LOG" >&2
    exit 1
fi
if [ ! -f "$ROM" ]; then
    echo "vfio-check-rom: no such ROM: $ROM" >&2
    echo "vfio-check-rom: run 'sudo scripts/vfio-vbios.sh' on the host first" >&2
    exit 1
fi

line="$(grep -o 'vfio rom crc32=0x[0-9a-fA-F]* size=[0-9]*' "$LOG" | tail -1 || true)"
if [ -z "$line" ]; then
    echo "vfio-check-rom: no 'vfio rom crc32=' line in $LOG" >&2
    exit 1
fi

guest_crc="$(printf '%s\n' "$line" | sed -n 's/.*crc32=0x\([0-9a-fA-F]*\).*/\1/p')"
guest_size="$(printf '%s\n' "$line" | sed -n 's/.*size=\([0-9]*\).*/\1/p')"
host_crc="$(python3 -c 'import sys,zlib; print("%08x" % (zlib.crc32(open(sys.argv[1],"rb").read()) & 0xffffffff))' "$ROM")"
host_size="$(stat -c %s "$ROM")"

echo "guest: crc32=0x$guest_crc size=$guest_size"
echo "host : crc32=0x$host_crc size=$host_size"
if [ "$guest_crc" = "$host_crc" ] && [ "$guest_size" = "$host_size" ]; then
    echo "MATCH"
else
    echo "MISMATCH" >&2
    exit 1
fi
