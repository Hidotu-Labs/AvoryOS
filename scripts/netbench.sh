#!/usr/bin/env bash
# netbench.sh - helper for AvoryOS network benchmarks (Phase 0 harness).
#
# The QEMU user network maps the host to 10.0.2.2 inside the guest, so the
# host runs a test server and the guest runs /bin/nettest against it.
#
# Usage:
#   ./scripts/netbench.sh server <tcp-sink|tcp-echo|tcp-source|udp-echo> PORT
#   ./scripts/netbench.sh guest [HOST]     print the guest-side command list
#   ./scripts/netbench.sh run [tcp-sink|tcp-echo|udp-echo] [PORT]
#                                          start the server, then boot QEMU
#
# Typical session (two terminals):
#   term A: ./scripts/netbench.sh server tcp-sink 9000
#   term B: make run RUN_NIC=e1000
#   guest:  nettest tcp-source 10.0.2.2 9000 10
#
# Results are saved manually; keep a build/netbench/ directory for baselines.

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SERVER="$ROOT_DIR/scripts/nettest-server.py"
HOST_IP="${HOST_IP:-10.0.2.2}"

usage() {
    sed -n '2,20p' "$0" | sed 's/^# \{0,1\}//'
    exit 2
}

case "${1:-}" in
server)
    mode="${2:-tcp-sink}"
    port="${3:-9000}"
    exec python3 "$SERVER" "$mode" "$port" --host 0.0.0.0
    ;;
guest)
    host="${2:-$HOST_IP}"
    cat <<EOF
Guest-side commands (run inside AvoryOS shell):

  # single-stream TCP throughput (host runs: netbench.sh server tcp-sink 9000)
  nettest tcp-source $host 9000 10

  # parallel streams: run a few times in the background via sh -c '... &'
  nettest tcp-source $host 9000 10 &

  # connection churn (host: tcp-sink 9000)
  nettest tcp-churn $host 9000 1000 32

  # websocket-style: 200 held connections, small messages (host: tcp-echo 9001)
  nettest ws-sim $host 9001 200 30

  # UDP voice profile (host: udp-echo 9002)
  nettest udp-voice $host 9002 30

  # UDP burst (host: udp-echo 9002)
  nettest udp-load $host 9002 10000 512
EOF
    ;;
run)
    mode="${2:-tcp-sink}"
    port="${3:-9000}"
    python3 "$SERVER" "$mode" "$port" --host 0.0.0.0 &
    srv_pid=$!
    trap 'kill $srv_pid 2>/dev/null || true' EXIT
    sleep 0.3
    echo "host server started (pid $srv_pid), booting QEMU..."
    make -C "$ROOT_DIR" "${RUN_TARGET:-run}" "RUN_NIC=${RUN_NIC:-rtl8139}"
    ;;
*)
    usage
    ;;
esac
