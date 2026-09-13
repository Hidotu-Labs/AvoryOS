#!/usr/bin/env bash
# kpi-triage.sh -- bucket the Phase 6 C2 compile diagnostics.
#
# Usage:
#   scripts/kpi-triage.sh                 # build with KPI_AMDGPU=1 and analyze
#   scripts/kpi-triage.sh <logfile>       # analyze an existing log
#
# Writes build/p6a-gaps.md (per-bucket counts + samples, ready to be folded
# into docs/linuxkpi-gaps.md) and prints a summary.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
LOG="${1:-}"
TOP="${TOP:-40}"

if [ -z "$LOG" ]; then
    mkdir -p "$ROOT/build/logs"
    LOG="$ROOT/build/logs/p6a-build.log"
    echo "[triage] building kernel with KPI_AMDGPU=1 (log: $LOG)"
    make -C "$ROOT/kernel" KPI_AMDGPU=1 -k -j"$(nproc)" >"$LOG" 2>&1 || true
elif [ ! -f "$LOG" ]; then
    echo "kpi-triage: no such log: $LOG" >&2
    exit 2
fi

OUT="$ROOT/build/p6a-gaps.md"
{
    echo "# Phase 6 C2 triage ($(date -u +%Y-%m-%dT%H:%M:%SZ))"
    echo
    echo "Source log: \`$LOG\`"
    echo
    echo "## Missing headers"
    echo '```'
    grep -hoE 'fatal error: [^:]+: No such file' "$LOG" 2>/dev/null \
        | sed -E 's/fatal error: //; s/: No such file$//' \
        | sort | uniq -c | sort -rn | head -"$TOP" || true
    echo '```'
    echo
    echo "## Undefined references (link)"
    echo '```'
    grep -hoE "undefined reference to [\`'][^\`']+[\`']" "$LOG" 2>/dev/null \
        | sort | uniq -c | sort -rn | head -"$TOP" || true
    echo '```'
    echo
    echo "## Implicit declarations"
    echo '```'
    grep -hoE "implicit declaration of function [\`'][^\`']+[\`']" "$LOG" 2>/dev/null \
        | sort | uniq -c | sort -rn | head -"$TOP" || true
    echo '```'
    echo
    echo "## Top error messages (normalized)"
    echo '```'
    grep -h 'error: ' "$LOG" 2>/dev/null \
        | sed -E 's/^[^:]+:[0-9]+:[0-9]+: //; s/ \(first use in this function\)//; s/ \[-W[^]]*\]//' \
        | sort | uniq -c | sort -rn | head -"$TOP" || true
    echo '```'
    echo
    echo "## Failing translation units"
    echo '```'
    grep -hE '^(linux|linuxkpi)/[^:]+:.*error:' "$LOG" 2>/dev/null \
        | awk -F: '{print $1}' | sort | uniq -c | sort -rn | head -"$TOP" || true
    echo '```'
} > "$OUT"

echo "[triage] wrote $OUT"
echo "[triage] $(grep -c 'error: ' "$LOG" 2>/dev/null || :) error line(s), $(grep -c 'undefined reference' "$LOG" 2>/dev/null || :) undefined reference(s)"
sed -n '1,50p' "$OUT"
