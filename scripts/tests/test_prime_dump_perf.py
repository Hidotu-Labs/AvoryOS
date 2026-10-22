#!/usr/bin/env python3
"""A host timing request must not request a hang dump or freeze the recorder."""
from pathlib import Path
import socket
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
with tempfile.TemporaryDirectory(prefix='avory-perf-socket-') as temporary:
    endpoint = Path(temporary)/'serial.sock'
    with socket.socket(socket.AF_UNIX) as server:
        server.bind(str(endpoint))
        server.listen(1)
        server.settimeout(5)
        process = subprocess.Popen(['python3',str(ROOT/'scripts/prime-dump.py'),
                                    '--socket',str(endpoint),'--perf'],stdout=subprocess.PIPE,
                                    stderr=subprocess.PIPE,text=True)
        connection, _ = server.accept()
        with connection:
            connection.settimeout(5)
            assert connection.recv(64) == b'gpup\n'
        output, error = process.communicate(timeout=5)
        assert process.returncode == 0, error
        assert 'GPU timing snapshot requested' in output
print('PASS: host --perf sends only the compact timing request')
