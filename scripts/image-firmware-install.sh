#!/usr/bin/env bash
# Install firmware after the rootfs overlay and /lib compatibility setup.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
IMAGE="${1:?usage: image-firmware-install.sh IMAGE [FIRMWARE_DIR TEST_BLOB]}"
SOURCE="${2:-$ROOT/build/firmware/lib/firmware}"
TEST_BLOB="${3:-$ROOT/build/test_fw.bin}"

[[ -d "$SOURCE" && -f "$TEST_BLOB" ]] || {
    echo 'Firmware staging directory or test blob is missing' >&2
    exit 1
}
STAGE="$(mktemp -d)"
trap 'rm -rf "$STAGE"' EXIT
mkdir -p "$STAGE/lib/firmware"
cp -a "$SOURCE/." "$STAGE/lib/firmware/"
cp "$TEST_BLOB" "$STAGE/lib/firmware/test_fw.bin"

bash "$ROOT/scripts/populate-ext2-dir.sh" "$IMAGE" "$STAGE/lib/firmware" /lib/firmware
# debugfs can return success for failed commands. Read every installed file
# through guest symlinks and compare its bytes before accepting the image.
paths=()
while IFS= read -r -d '' file; do
    paths+=("/${file#"$STAGE/"}")
done < <(find "$STAGE/lib/firmware" -type f -print0)
python3 "$ROOT/scripts/image-runtime.py" "$IMAGE" "$STAGE" "${paths[@]}"
