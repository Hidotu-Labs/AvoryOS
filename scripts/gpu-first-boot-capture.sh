#!/bin/sh
# Guest diagnostic: compare two 3D oracle runs without rebooting the GPU.
# No global Mesa overrides, cache deletion, game changes or reset operations.
set -eu
label=${1:-unlabelled}
cache=${2:-default}
case "$label" in ''|*[!a-zA-Z0-9_-]*) echo 'label must contain letters, numbers, _ or -' >&2; exit 2;; esac
case "$cache" in default|disabled) ;; *) echo 'usage: sh gpu-first-boot-capture.sh [label] [default|disabled]' >&2; exit 2;; esac
work=$(mktemp -d /tmp/gpu-first-boot.XXXXXX)
base=http://10.0.2.2:9007
curl -f "$base/test_gpu_pipeline_gl" -o "$work/probe"
chmod +x "$work/probe"
{
 echo "GPU_FIRST_BOOT: label=$label cache=$cache directory=$work"
 echo 'GPU_FIRST_BOOT: existing diagnostic environment (no values changed globally)'
 env | sed -n '/^AMD_DEBUG=/p; /^R600_DEBUG=/p; /^GALLIUM_DRIVER=/p; /^LIBGL_ALWAYS_SOFTWARE=/p; /^MESA_SHADER_CACHE/p'
 echo 'GPU_FIRST_BOOT: renderer must say RadeonSI; software or incomplete checks do not validate hardware'
} > "$work/report.log"
for pass in 1 2; do
 echo "GPU_FIRST_BOOT: BEGIN pass=$pass label=$label cache=$cache" >> "$work/report.log"
 # Preserve failure output and still run the second comparison. Exit=2 may
 # mean incomplete coverage, including no observed submission overlap.
 rc=0
 if [ "$cache" = disabled ]; then
  env MESA_SHADER_CACHE_DISABLE=true "$work/probe" > "$work/pass$pass.log" 2>&1 || rc=$?
 else
  "$work/probe" > "$work/pass$pass.log" 2>&1 || rc=$?
 fi
 cat "$work/pass$pass.log" >> "$work/report.log"
 echo "GPU_FIRST_BOOT: END pass=$pass exit=$rc" >> "$work/report.log"
done
cat "$work/report.log"
if [ -w /dev/ttyS0 ]; then
 cat "$work/report.log" > /dev/ttyS0
 echo 'Report also sent to the host serial log.'
fi
echo "Saved report: $work/report.log"
echo 'Check the affected app again: did these draws change the bad pixels without a VM restart?'
