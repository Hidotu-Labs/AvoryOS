#!/usr/bin/env bash
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"
KERNEL=${1:?usage: create_nouveau_c4_iso.sh KERNEL OUTPUT.iso CMDLINE}
OUTPUT=${2:?usage: create_nouveau_c4_iso.sh KERNEL OUTPUT.iso CMDLINE}
CMDLINE=${3:?usage: create_nouveau_c4_iso.sh KERNEL OUTPUT.iso CMDLINE}
ISO_ROOT=build/nouveau-618/limine-iso-root

for file in "$KERNEL" limine.conf limine/limine limine/limine-bios.sys \
            limine/limine-bios-cd.bin limine/limine-uefi-cd.bin \
            limine/BOOTX64.EFI assets/boo.png; do
    [[ -f $file ]] || { echo "required Limine ISO input missing: $file" >&2; exit 1; }
done

rm -rf "$ISO_ROOT"
mkdir -p "$ISO_ROOT/boot/limine" "$ISO_ROOT/EFI/BOOT"
cp "$KERNEL" "$ISO_ROOT/boot/kernel"
strip --strip-all "$ISO_ROOT/boot/kernel"
cp assets/boo.png "$ISO_ROOT/boot/limine/"
cp limine/limine-bios.sys limine/limine-bios-cd.bin \
   limine/limine-uefi-cd.bin "$ISO_ROOT/boot/limine/"
cp limine/BOOTX64.EFI "$ISO_ROOT/EFI/BOOT/"
cp limine/BOOTIA32.EFI "$ISO_ROOT/EFI/BOOT/" 2>/dev/null || true

# Match the ordinary Limine ISO's external writable disk layout. Add C4
# parameters only to the installed AvoryOS entry; preserve the other entries.
sed '/^[[:space:]]*module_path: boot():\/disk.img$/d; /^[[:space:]]*module_string: disk.img$/d' limine.conf |
awk -v cmdline="$CMDLINE" '
    /^\/AvoryOS \(OpenRC\)$/ { target=1; print; next }
    /^\// { target=0 }
    target && /^[[:space:]]*cmdline:/ { print "    cmdline: " cmdline; target=0; next }
    { print }
' > "$ISO_ROOT/boot/limine/limine.conf"

xorriso -as mkisofs -R -r -J -b boot/limine/limine-bios-cd.bin \
    -no-emul-boot -boot-load-size 4 -boot-info-table -hfsplus \
    -apm-block-size 2048 --efi-boot boot/limine/limine-uefi-cd.bin \
    -efi-boot-part --efi-boot-image --protective-msdos-label \
    "$ISO_ROOT" -o "$OUTPUT"
limine/limine bios-install "$OUTPUT"
rm -rf "$ISO_ROOT"
echo "Created standard Limine C4 image: $OUTPUT"
