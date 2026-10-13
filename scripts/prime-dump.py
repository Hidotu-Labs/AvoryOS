#!/usr/bin/env python3
"""Ask a running run-c7-prime VM to append a thread dump to its serial log."""
from pathlib import Path
import socket
import sys

root = Path(__file__).resolve().parents[1]
endpoint = root / 'build/logs/p6-c7-prime-serial.sock'
try:
    with socket.socket(socket.AF_UNIX) as connection:
        connection.settimeout(3)
        connection.connect(str(endpoint))
        connection.sendall(b'dump\n')
except OSError as error:
    sys.exit(f'Cannot reach the PRIME VM at {endpoint}: {error}')
print(f'Thread dump requested; inspect {root / "build/logs/p6-c7-prime.log"}')
