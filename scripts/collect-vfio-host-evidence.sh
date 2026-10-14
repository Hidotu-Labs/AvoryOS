#!/usr/bin/env bash
# Read-only evidence collection. No device binding, reset, PCI/configuration
# writes, power changes, QEMU launches or host restarts.
set -u
umask 077
out=$(mktemp -d /tmp/avory-host-evidence.XXXXXXXX)
status=0

capture() {
    local name=$1
    shift
    if ! "$@" > "$out/$name" 2>&1; then
        printf 'Unavailable: %s\n' "$name" >> "$out/unavailable.txt"
        status=1
    fi
}

capture host.txt uname -a
capture boots.txt journalctl --list-boots --no-pager
capture previous-kernel.txt journalctl -b -1 -k --no-pager
capture current-kernel.txt journalctl -b 0 -k --no-pager
capture dmesg.txt dmesg
capture gpu-pci.txt lspci -nnk -s 0e:00
capture bridge-pci.txt lspci -nnk -s 00:08.1
capture gpu-capabilities.txt lspci -vv -s 0e:00.0

{
    for bdf in 0000:0e:00.{0,1,2,3,4,6} 0000:00:08.1; do
        dev=/sys/bus/pci/devices/$bdf
        printf '%s\n' "$bdf"
        for link in driver iommu_group; do
            printf '  %s: ' "$link"
            readlink -f "$dev/$link" || true
        done
        for attr in reset_method d3cold_allowed power_state power/runtime_status power/control; do
            if [[ -r $dev/$attr ]]; then
                printf '  %s: ' "$attr"
                cat "$dev/$attr"
            fi
        done
    done
} > "$out/topology.txt" 2>&1

# Read firmware descriptions only; never evaluate an ACPI method or change
# device power. _PR3/_PS3 must be inspected before proposing a power cycle.
{
    for bdf in 0000:0e:00.0 0000:00:08.1; do
        node=/sys/bus/pci/devices/$bdf/firmware_node
        printf '%s ACPI companion: ' "$bdf"
        readlink -f "$node" || true
        if [[ -r $node/path ]]; then cat "$node/path"; fi
        for directory in "$node"/power_resources* "$node"/LNXPOWER:*; do
            [[ -d $directory ]] || continue
            printf '  resource directory: %s\n' "$directory"
            for attr in path resource_in_use; do
                if [[ -r $directory/$attr ]]; then
                    printf '    %s: ' "$attr"
                    cat "$directory/$attr"
                fi
            done
            for link in "$directory"/*; do
                [[ -L $link ]] || continue
                printf '    link: %s -> ' "${link##*/}"
                readlink -f "$link" || true
            done
        done
    done
} > "$out/acpi-power-resources.txt" 2>&1
mkdir "$out/acpi-tables"
for table in /sys/firmware/acpi/tables/DSDT /sys/firmware/acpi/tables/SSDT*; do
    [[ -f $table ]] || continue
    if [[ -r $table ]]; then
        cp -- "$table" "$out/acpi-tables/" || status=1
    else
        printf 'Unavailable: ACPI table %s (requires privileged read)\n' "${table##*/}" >> "$out/unavailable.txt"
        status=1
    fi
done

mkdir "$out/pstore"
if [[ -r /sys/fs/pstore && -x /sys/fs/pstore ]]; then
    for record in /sys/fs/pstore/*; do
        [[ -f $record ]] || continue
        cp -- "$record" "$out/pstore/" || status=1
    done
else
    printf 'Unavailable: pstore permission\n' >> "$out/unavailable.txt"
    status=1
fi

# When invoked through sudo, allow the calling user to inspect this fresh
# private directory. Accept numeric identities only; never change source files.
if [[ ${EUID} == 0 && ${SUDO_UID:-} =~ ^[0-9]+$ && ${SUDO_GID:-} =~ ^[0-9]+$ ]]; then
    chown -R "${SUDO_UID}:${SUDO_GID}" "$out"
fi
printf 'Evidence directory: %s\n' "$out"
if ((status)); then
    printf 'Some records were unavailable; see unavailable.txt.\n'
fi
exit "$status"
