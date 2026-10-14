#!/usr/bin/env python3
"""Check launch evidence using disposable CPU processes, never VFIO."""
from pathlib import Path
import json
import os
import signal
import subprocess
import sys
import tempfile
import time

ROOT = Path(__file__).resolve().parents[2]
with tempfile.TemporaryDirectory(prefix='qemu-prime-log-') as temporary:
    directory = Path(temporary)
    serial = directory / 'serial.log'
    serial.write_bytes(b'PRIOR ATTEMPT\n')
    runs = directory / 'runs'
    launch = [sys.executable, str(ROOT / 'scripts/qemu-prime-log.py'),
              '--output-root', str(runs), '--serial-log', str(serial), '--']
    child = [sys.executable, '-c',
             'import os,sys;'
             'open(sys.argv[1],"ab").write(b"GUEST DATA\\n");'
             'os.write(2,b"DEVICE OPEN ERROR\\n");sys.exit(7)', str(serial)]
    result = subprocess.run(launch + child, capture_output=True, timeout=10)
    assert result.returncode == 7, result.stderr
    run = next(runs.iterdir())
    data = json.loads((run / 'launch.json').read_text())
    assert data['status'] == 'exited' and data['returncode'] == 7
    assert serial.read_bytes().startswith(b'PRIOR ATTEMPT\n')
    assert b'GUEST DATA' in (run / 'serial.log').read_bytes()
    assert b'PRIOR ATTEMPT' not in (run / 'serial.log').read_bytes()
    assert (run / 'qemu-stderr.log').read_bytes() == b'DEVICE OPEN ERROR\n'
    assert b'DEVICE OPEN ERROR' in result.stderr
    assert (run.stat().st_mode & 0o777) == 0o700
    # Lock ownership prevents concurrent launches; SIGTERM reaches the child.
    process = subprocess.Popen(launch + [sys.executable, '-c', 'import time;time.sleep(60)'],
                               stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    try:
        deadline = time.monotonic() + 5
        second = None
        while time.monotonic() < deadline:
            candidates = [p for p in runs.iterdir() if p != run]
            if candidates:
                second = candidates[0]
                metadata = json.loads((second / 'launch.json').read_text())
                if metadata['status'] == 'running':
                    break
            time.sleep(.02)
        else:
            raise AssertionError('test child did not start')
        sentinel = directory / 'must-not-start'
        rejected = subprocess.run(launch + [sys.executable, '-c',
                                  'import pathlib,sys;pathlib.Path(sys.argv[1]).touch()',
                                  str(sentinel)], capture_output=True, timeout=10)
        assert rejected.returncode != 0 and not sentinel.exists()
        assert b'refusing to start a second VM' in rejected.stderr
        process.send_signal(signal.SIGTERM)
        stdout, stderr = process.communicate(timeout=10)
        assert process.returncode == 128 + signal.SIGTERM, stderr
        final = json.loads((second / 'launch.json').read_text())
        assert final['returncode'] == -signal.SIGTERM and final['status'] == 'exited'
        try:
            os.kill(final['qemu_pid'], 0)
        except ProcessLookupError:
            pass
        else:
            raise AssertionError('child was left running')
    finally:
        if process.poll() is None:
            process.kill()
            process.wait()
    print('PASS: prior serial preserved, per-launch metadata/serial/stderr captured, '
          'exit status preserved, concurrent launch rejected and termination forwarded')
