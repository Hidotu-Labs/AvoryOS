#!/usr/bin/env python3
"""Ask a running run-c7-prime VM to append a thread dump to its serial log."""
from pathlib import Path
import argparse
import socket
import sys

root = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--perf', action='store_true', help='request a compact GPU timing snapshot without freezing the flight recorder')
target = parser.add_mutually_exclusive_group()
target.add_argument('--nouveau', action='store_true', help='request a dump from run-nouveau-c4')
target.add_argument('--socket', type=Path, help='explicit diagnostic serial socket')
args = parser.parse_args()
endpoint = args.socket or root / ('build/logs/nouveau-c4-serial.sock' if args.nouveau else 'build/logs/p6-c7-prime-serial.sock')
try:
    with socket.socket(socket.AF_UNIX) as connection:
        connection.settimeout(3)
        connection.connect(str(endpoint))
        connection.sendall(b'gpup\n' if args.perf else b'dump\n')
except OSError as error:
    sys.exit(f'Cannot reach the PRIME VM at {endpoint}: {error}')
log = root / ('build/logs/nouveau-c4.log' if args.nouveau else 'build/logs/p6-c7-prime.log')
kind = 'GPU timing snapshot' if args.perf else 'Thread dump'
print(f'{kind} requested through {endpoint}; inspect {log}')
