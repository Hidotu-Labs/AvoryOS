#!/bin/sh
# Guest command: wait for the game, record native metadata/pixel checks, freeze
# to serial. Does not alter Minecraft settings, Mesa, or GPU driver options.
set -eu
delay=${1:-5}
seconds=${2:-15}
mode=${3:-1}
output=${4:-/tmp/gpu-debug-capture.log}
case "$delay:$seconds:$mode" in *[!0-9:]*|'') echo "usage: $0 [delay=5] [seconds=15] [mode=1|2|3] [output=/tmp/gpu-debug-capture.log]" >&2; exit 2;; esac
case "$mode" in 1|2|3) ;; *) echo "mode must be 1 (metadata), 2 (+64 PRIME pixel checks), or 3 (+1024 resource events)" >&2; exit 2;; esac
[ -w /proc/gpu_debug ] || { echo "Boot the diagnostic ISO and run as root (/proc/gpu_debug required)." >&2; exit 1; }
echo "Switch back to the affected program and keep it running; recording begins in $delay seconds."
sleep "$delay"
echo "$mode" > /proc/gpu_debug
sleep "$seconds"
echo freeze > /proc/gpu_debug
cat /proc/gpu_debug > "$output"
echo dump > /proc/gpu_debug
echo "Frozen GPU diagnostics saved to $output and exported to the host serial log."
