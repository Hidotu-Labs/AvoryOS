#!/usr/bin/env bash
# run-c6-watchdog.sh -- boot `make run-c6` headless and stop QEMU at the first
# terminal marker in the serial log (login prompt, fatal GPU-init failure, or
# an IB-ring failure) instead of leaving QEMU running forever.
#
# The DC-on probe is minutes-slow in this environment (DCN bring-up), so the
# default timeout is generous.  Usage: scripts/run-c6-watchdog.sh [timeout_s]
# The serial log is build/logs/p6-c6.log (written by `make run-c6`).
set -u

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"
TIMEOUT="${1:-2400}"
LOG="build/logs/p6-c6.log"

rm -f "$LOG"
make run-c6 DISPLAY_OPT='-display none' &
MAKE_PID=$!

result="timeout"
deadline=$((SECONDS + TIMEOUT))
while kill -0 "$MAKE_PID" 2>/dev/null; do
    if [ "$SECONDS" -ge "$deadline" ]; then
        result="timeout after ${TIMEOUT}s"
        break
    fi
    if [ -f "$LOG" ]; then
        if grep -q "AvoryOS login" "$LOG"; then
            result="login reached"
            sleep 5
            break
        fi
        if grep -q "Fatal error during GPU init" "$LOG"; then
            result="GPU init failed"
            sleep 5
            break
        fi
        if grep -q "hw_init of IP block <dm> failed" "$LOG"; then
            result="dm ip block hw_init failed"
            sleep 5
            break
        fi
        if grep -q "ib ring test failed" "$LOG"; then
            result="gfx IB test failed (-110)"
            sleep 10
            break
        fi
    fi
    sleep 2
done

pkill -f "avoryos-x86_64.iso" 2>/dev/null || true
kill "$MAKE_PID" 2>/dev/null || true
wait "$MAKE_PID" 2>/dev/null || true
echo "run-c6-watchdog: stopped ($result)"
