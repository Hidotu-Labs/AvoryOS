#!/bin/sh
# Replay the already captured GLX stream, collecting framebuffer snapshots.
set -u
trace=
for candidate in /tmp/tunnel-capture.*/tunnel.trace; do
    [ -s "$candidate" ] || continue
    if [ -z "$trace" ] || [ "$candidate" -nt "$trace" ]; then
        trace=$candidate
    fi
done
if [ -z "$trace" ]; then
    echo 'No existing tunnel trace found; run the capture helper first.'
    exit 1
fi
work=$(mktemp -d /tmp/tunnel-replay.XXXXXX) || exit 1
curl -fsS http://10.0.2.2:9007/glretrace -o "$work/glretrace" || exit 1
chmod +x "$work/glretrace" || exit 1
echo 'Replaying the saved tunnel trace on AMD; snapshots upload automatically.'
env DRI_PRIME=pci-0000_00_03_0 "$work/glretrace" --snapshot-interval=10 -s - "$trace" > "$work/frames.pnm" 2> "$work/replay.log"
rc=$?
echo "Replay exit status: $rc"
cat "$work/replay.log"
curl -fsS --data-binary "@$work/replay.log" http://10.0.2.2:9008/tunnel-replay-log
# Check the renderer from this replay's actual GL context. Device selection
# can fail and Mesa can successfully fall back to llvmpipe with exit status 0.
if ! grep -E '^GL_RENDERER:.*radeonsi' "$work/replay.log" > /dev/null; then
    echo "AMD replay was not established; log saved at $work/replay.log"
    echo 'Software or another GPU snapshots are not AMD validation.'
    exit 2
fi
if [ ! -s "$work/frames.pnm" ]; then
    echo "No snapshots created; log saved at $work/replay.log"
    exit 1
fi
if ! curl -f --data-binary "@$work/frames.pnm" http://10.0.2.2:9008/tunnel-frames; then
    echo "Snapshots saved at $work/frames.pnm"
    exit 1
fi
exit "$rc"
