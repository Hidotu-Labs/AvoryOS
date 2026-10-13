#!/bin/sh
# Run in the guest X11 desktop. Preserve results if the upload fails.
set -u
work=$(mktemp -d /tmp/tunnel-capture.XXXXXX) || exit 1
curl -fsS http://10.0.2.2:9007/glxtrace.so -o "$work/glxtrace.so" || exit 1
if ! command -v tunnel >/dev/null 2>&1; then
    echo 'Cannot find tunnel.'
    exit 1
fi
echo 'Capturing tunnel for five seconds; the window will close automatically.'
timeout -k 2s 5s env TRACE_FILE="$work/tunnel.trace" LD_PRELOAD="$work/glxtrace.so" tunnel > "$work/tunnel.log" 2>&1
rc=$?
printf 'timeout_status=%s (124 is the expected timed stop)\n' "$rc" > "$work/status.txt"
cat "$work/tunnel.log"
if [ ! -s "$work/tunnel.trace" ]; then
    echo "No trace was created; log saved at $work/tunnel.log"
    curl -fsS --data-binary "@$work/tunnel.log" http://10.0.2.2:9008/tunnel-log
    exit 1
fi
curl -fsS --data-binary "@$work/tunnel.log" http://10.0.2.2:9008/tunnel-log
if curl -f --data-binary "@$work/tunnel.trace" http://10.0.2.2:9008/tunnel-trace; then
    echo 'Tunnel capture received on host.'
else
    echo "Capture saved at $work/tunnel.trace"
    exit 1
fi
