#!/usr/bin/env bash
# Verify the startup files in the ext4 partition rather than the staging tree.
set -euo pipefail

root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
image=${1:?usage: verify-desktop-image.sh PARTITION}
scratch=$(mktemp -d)
trap 'rm -rf "$scratch"' EXIT

read_image_file() {
    rm -f "$scratch/file"
    debugfs -R "dump $1 $scratch/file" "$image" >/dev/null 2>&1
    if [ ! -s "$scratch/file" ]; then
        echo "Desktop image is missing $1" >&2
        exit 1
    fi
}

compare_image_file() {
    read_image_file "$1"
    if ! cmp -s "$scratch/file" "$root/$2"; then
        echo "Desktop image has a stale $1" >&2
        exit 1
    fi
}

compare_image_file /etc/systemd/system/avory-login.service config/systemd/avory-login.service
compare_image_file /etc/xdg/plasma-workspace/env/avory-pipewire-systemd.sh config/plasma/avory-pipewire-systemd.sh

sessions=/etc/systemd/system/systemd-user-sessions.service
read_image_file "$sessions"
cp "$scratch/file" "$scratch/override"
read_image_file /usr/lib/systemd/system/systemd-user-sessions.service
sed '/^After=/s/ network\.target\([[:space:]]\|$\)/\1/' "$scratch/file" >"$scratch/expected"
if ! cmp -s "$scratch/expected" "$scratch/override"; then
    echo "Desktop image has a stale $sessions" >&2
    exit 1
fi
