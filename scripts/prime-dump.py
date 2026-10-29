#!/usr/bin/env python3
"""Ask a running run-amd-gpu VM to append a thread dump to its serial log."""
from pathlib import Path
import argparse
import socket
import sys

root = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--log', type=Path, help='serial log override, useful with --socket')
target = parser.add_mutually_exclusive_group()
target.add_argument('--nouveau', action='store_true', help='request a dump from run-nouveau')
target.add_argument('--socket', type=Path, help='explicit diagnostic serial socket')
args = parser.parse_args()
log = args.log or root / ('build/logs/nouveau-c4.log' if args.nouveau else 'build/logs/p6-c7-prime.log')
endpoint = args.socket or root / ('build/logs/nouveau-c4-serial.sock' if args.nouveau else 'build/logs/p6-c7-prime-serial.sock')
try:
    with socket.socket(socket.AF_UNIX) as connection:
        connection.settimeout(3)
        connection.connect(str(endpoint))
        connection.sendall(b'dump\n')
except OSError as error:
    sys.exit(f'Cannot reach the PRIME VM at {endpoint}: {error}')
print(f'Thread dump requested through {endpoint}; inspect {log}')
