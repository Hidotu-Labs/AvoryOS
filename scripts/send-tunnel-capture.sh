#!/bin/sh
# Recover the newest existing capture without rerunning the demo.
set -u
trace=
for candidate in /tmp/tunnel-capture.*/tunnel.trace; do
    [ -s "$candidate" ] || continue
    if [ -z "$trace" ] || [ "$candidate" -nt "$trace" ]; then
        trace=$candidate
    fi
done
if [ -z "$trace" ]; then
    echo 'No saved tunnel trace found.'
    exit 1
fi
echo "Sending $trace"
log=${trace%/*}/tunnel.log
if [ -f "$log" ]; then
    curl -fsS --data-binary "@$log" http://10.0.2.2:9008/tunnel-log
fi
curl -f --data-binary "@$trace" http://10.0.2.2:9008/tunnel-trace
