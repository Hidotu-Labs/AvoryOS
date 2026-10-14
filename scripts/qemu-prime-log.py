#!/usr/bin/env python3
"""Run one explicitly requested QEMU command and retain durable launch logs.

Never retries, resets devices, changes host settings, or starts a second VM.
The QEMU serial chardev must use logappend=on (or file append=on).
"""
import argparse
from datetime import datetime, timezone
import fcntl
import hashlib
import json
import os
from pathlib import Path
import selectors
import signal
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def sync_json(path, data):
    temporary = path.with_name('.' + path.name + '.tmp')
    with temporary.open('w') as stream:
        json.dump(data, stream, indent=2)
        stream.write('\n')
        stream.flush()
        os.fsync(stream.fileno())
    os.replace(temporary, path)
    directory = os.open(path.parent, os.O_RDONLY | os.O_DIRECTORY)
    os.fsync(directory)
    os.close(directory)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output-root', type=Path, default=ROOT / 'build/logs/prime-runs')
    parser.add_argument('--serial-log', type=Path, default=ROOT / 'build/logs/p6-c7-prime.log')
    parser.add_argument('command', nargs=argparse.REMAINDER)
    args = parser.parse_args()
    command = args.command[1:] if args.command[:1] == ['--'] else args.command
    if not command:
        parser.error('a QEMU command after -- is required')
    args.output_root.mkdir(parents=True, exist_ok=True)
    args.serial_log.parent.mkdir(parents=True, exist_ok=True)
    with (args.serial_log.parent / (args.serial_log.name + '.launch-lock')).open('a') as lock:
        try:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError:
            parser.error('another launch owns this serial log; refusing to start a second VM')
        now = datetime.now(timezone.utc)
        run = Path(tempfile.mkdtemp(prefix=now.strftime('%Y%m%dT%H%M%SZ-'),
                                   dir=args.output_root))
        os.chmod(run, 0o700)
        metadata = {'started_utc': now.isoformat(), 'status': 'launching',
                    'host_boot_id': Path('/proc/sys/kernel/random/boot_id').read_text().strip(),
                    'serial_log': str(args.serial_log.resolve())}
        if '-cdrom' in command:
            iso = Path(command[command.index('-cdrom') + 1])
            with iso.open('rb') as stream:
                metadata['iso_sha256'] = hashlib.file_digest(stream, 'sha256').hexdigest()
        sync_json(run / 'launch.json', metadata)
        directory = os.open(args.output_root, os.O_RDONLY | os.O_DIRECTORY)
        os.fsync(directory)
        os.close(directory)
        with args.serial_log.open('ab', buffering=0) as serial, \
                (run / 'serial.log').open('ab', buffering=0) as copied, \
                (run / 'qemu-stderr.log').open('ab', buffering=0) as errors:
            offset = serial.tell()
            serial.write(f'\n[QEMU-LAUNCH] {now.isoformat()} run={run.name}\n'.encode())
            os.fsync(serial.fileno())
            reader = args.serial_log.open('rb', buffering=0)
            reader.seek(offset)
            process = None
            cancelled = None

            def forward(signum, _frame):
                nonlocal cancelled
                cancelled = signum
                if process is not None and process.poll() is None:
                    process.send_signal(signum)

            def copy_serial():
                while chunk := reader.read(65536):
                    copied.write(chunk)
                    os.fsync(copied.fileno())
                    os.fsync(serial.fileno())

            signal.signal(signal.SIGTERM, forward)
            signal.signal(signal.SIGINT, forward)
            print(f'QEMU launch evidence: {run}', flush=True)
            try:
                # Flush the launch marker before opening a passed device.
                copy_serial()
                if cancelled is not None:
                    metadata.update(status='cancelled', signal=cancelled)
                    sync_json(run / 'launch.json', metadata)
                    return 128 + cancelled
                process = subprocess.Popen(command, stderr=subprocess.PIPE)
                if cancelled is not None and process.poll() is None:
                    process.send_signal(cancelled)
                metadata.update(status='running', qemu_pid=process.pid)
                sync_json(run / 'launch.json', metadata)
                os.set_blocking(process.stderr.fileno(), False)
                with selectors.DefaultSelector() as selector:
                    selector.register(process.stderr, selectors.EVENT_READ)
                    while True:
                        for key, _ in selector.select(.2):
                            chunk = os.read(key.fd, 65536)
                            if chunk:
                                errors.write(chunk)
                                os.fsync(errors.fileno())
                                sys.stderr.buffer.write(chunk)
                                sys.stderr.buffer.flush()
                            else:
                                selector.unregister(key.fileobj)
                        copy_serial()
                        if process.poll() is not None and not selector.get_map():
                            break
                metadata.update(status='exited', returncode=process.returncode,
                                ended_utc=datetime.now(timezone.utc).isoformat())
                sync_json(run / 'launch.json', metadata)
                process.stderr.close()
                return process.returncode if process.returncode >= 0 else 128 - process.returncode
            finally:
                reader.close()


if __name__ == '__main__':
    sys.exit(main())
