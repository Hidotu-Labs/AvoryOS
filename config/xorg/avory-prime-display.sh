#!/bin/sh
# Connect the render GPU to output-only GPUs before enabling their outputs.
# SDDM and console-started X sessions both need this step.
set -eu

providers=$(xrandr --listproviders) || exit 1
printf '%s\n' "$providers"
source=$(printf '%s\n' "$providers" | awk '/Source Output/ {print $4; exit}')
sinks=
if [ -n "$source" ]; then
    sinks=$(printf '%s\n' "$providers" | awk -v source="$source" '/Sink Output/ && $4 != source {print $4}')
    for sink in $sinks; do
        [ "$sink" != "$source" ] || continue
        xrandr --setprovideroutputsource "$sink" "$source"
    done
fi
if [ -z "$sinks" ]; then
    xrandr --auto
    exit
fi

# The VFIO profile exposes VirtIO's HDMI connector as HDMI-<port>-<GPU>.
# Enabling the synthetic AMD HDMI head as well routes Present to a head
# without a real monitor/vblank stream. Keep that head disabled.
outputs=$(xrandr --query) || exit 1
printf '%s\n' "$outputs"
virtual=$(printf '%s\n' "$outputs" | awk '$2 == "connected" && $1 ~ /^HDMI-[0-9]+-[0-9]+$/ {print $1; exit}')
[ -n "$virtual" ] || { echo 'PRIME: no connected VirtIO output' >&2; exit 1; }
set --
for output in $(printf '%s\n' "$outputs" | awk '$2 == "connected" || $2 == "disconnected" {print $1}'); do
    if [ "$output" = "$virtual" ]; then
        set -- "$@" --output "$output" --auto --primary --set "PRIME Synchronization" 0
    else
        set -- "$@" --output "$output" --off
    fi
done
xrandr "$@"
