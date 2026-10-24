#!/usr/bin/env python3
"""Ask a running run-amd-gpu VM to append a thread dump to its serial log."""
from pathlib import Path
import argparse
import socket
import sys
import time

root = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--perf', action='store_true', help='request a compact GPU timing snapshot without freezing the flight recorder')
parser.add_argument('--wait', action='store_true', help='with --perf, wait for and print one complete timing report')
parser.add_argument('--log', type=Path, help='serial log override, useful with --socket')
parser.add_argument('--wait-timeout', type=float, default=10, help='maximum seconds for --wait (default 10)')
target = parser.add_mutually_exclusive_group()
target.add_argument('--nouveau', action='store_true', help='request a dump from run-nouveau')
target.add_argument('--socket', type=Path, help='explicit diagnostic serial socket')
args = parser.parse_args()
if args.wait and not args.perf:
    parser.error('--wait requires --perf')
if args.wait_timeout <= 0:
    parser.error('--wait-timeout must be positive')
log = args.log or root / ('build/logs/nouveau-c4.log' if args.nouveau else 'build/logs/p6-c7-prime.log')
try:
    initial = log.stat()
    offset, identity = initial.st_size, initial.st_ino
except FileNotFoundError:
    offset, identity = 0, None
endpoint = args.socket or root / ('build/logs/nouveau-c4-serial.sock' if args.nouveau else 'build/logs/p6-c7-prime-serial.sock')
try:
    with socket.socket(socket.AF_UNIX) as connection:
        connection.settimeout(3)
        connection.connect(str(endpoint))
        connection.sendall(b'gpup\n' if args.perf else b'dump\n')
except OSError as error:
    sys.exit(f'Cannot reach the PRIME VM at {endpoint}: {error}')
kind = 'GPU timing snapshot' if args.perf else 'Thread dump'
print(f'{kind} requested through {endpoint}; inspect {log}')

if args.wait:
    deadline = time.monotonic() + args.wait_timeout
    pending = ''
    while time.monotonic() < deadline:
        try:
            status = log.stat()
            if identity != status.st_ino or status.st_size < offset:
                offset, pending = 0, ''
            identity = status.st_ino
            with log.open('rb') as source:
                source.seek(offset)
                pending += source.read().decode(errors='replace')
                offset = source.tell()
        except FileNotFoundError:
            pass
        start = pending.find('[GPUPERF] snapshot_ns=')
        end = pending.find('[GPUPERF] end', start) if start >= 0 else -1
        if end >= 0:
            print(pending[start:end + len('[GPUPERF] end')].replace('\r', ''))
            break
        time.sleep(0.05)
    else:
        sys.exit(f'Timing request sent, but no complete report arrived within {args.wait_timeout:g}s; inspect {log}')
