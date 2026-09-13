#!/usr/bin/env bash
# run-c4-watchdog.sh -- boot `make run-c4` and stop QEMU at the first
# terminal marker in the serial log (login prompt, gfx IB-test failure, or
# fatal GPU-init failure) instead of leaving QEMU running forever.
#
# Usage: scripts/run-c4-watchdog.sh [timeout_seconds]
# The serial log is build/logs/p6-c4.log (written by `make run-c4`).
set -u

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"
TIMEOUT="${1:-1500}"
LOG="build/logs/p6-c4.log"

rm -f "$LOG"
make run-c4 &
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
        if grep -q "ib ring test failed" "$LOG"; then
            result="gfx IB test failed (-110)"
            sleep 10
            break
        fi
        if grep -q "Fatal error during GPU init" "$LOG"; then
            result="GPU init failed"
            sleep 5
            break
        fi
    fi
    sleep 2
done

pkill -f "avoryos-x86_64.iso" 2>/dev/null || true
kill "$MAKE_PID" 2>/dev/null || true
wait "$MAKE_PID" 2>/dev/null || true
echo "run-c4-watchdog: stopped ($result)"
