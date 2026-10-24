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
from vfio_resume_check import check_raphael_resume

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


class GuestGpuExit:
    """Record observed guest callbacks, not a claim of hardware reset success."""
    def __init__(self):
        self.partial = b''
        self.initialized = False
        self.shutdown_started = False
        self.shutdown_completed = False

    def observe(self, chunk):
        lines = (self.partial + chunk).split(b'\n')
        self.partial = lines.pop()[-4096:]
        for line in lines:
            if b'[drm] Initialized amdgpu ' in line:
                # A guest reboot can initialize the device again in one run.
                self.initialized = True
                self.shutdown_started = self.shutdown_completed = False
            if b'LinuxKPI: shutting down pci amdgpu ' in line:
                self.shutdown_started = True
            if b'LinuxKPI: pci amdgpu ' in line and b' shutdown complete' in line:
                self.shutdown_completed = True

    def snapshot(self):
        return {'amdgpu_initialized': self.initialized,
                'amdgpu_shutdown_started': self.shutdown_started,
                'amdgpu_shutdown_callback_completed': self.shutdown_completed}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output-root', type=Path, default=ROOT / 'build/logs/prime-runs')
    parser.add_argument('--serial-log', type=Path, default=ROOT / 'build/logs/p6-c7-prime.log')
    parser.add_argument('--allow-resumed-raphael', action='store_true',
                        help='Explicit hardware diagnostic: permit the known host-reboot trigger')
    parser.add_argument('command', nargs=argparse.REMAINDER)
    args = parser.parse_args()
    command = args.command[1:] if args.command[:1] == ['--'] else args.command
    if not command:
        parser.error('a QEMU command after -- is required')
    try:
        resume_check = check_raphael_resume(command, allow_resumed=args.allow_resumed_raphael)
    except RuntimeError as error:
        parser.error(str(error))
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
                    'host_resume_check': resume_check,
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
            gpu_exit = GuestGpuExit()

            def forward(signum, _frame):
                nonlocal cancelled
                cancelled = signum
                if process is not None and process.poll() is None:
                    process.send_signal(signum)

            def copy_serial():
                while chunk := reader.read(65536):
                    gpu_exit.observe(chunk)
                    copied.write(chunk)
                    os.fsync(copied.fileno())
                    os.fsync(serial.fileno())

            signal.signal(signal.SIGTERM, forward)
            signal.signal(signal.SIGINT, forward)
            print(f'QEMU launch evidence: {run}', flush=True)
            if any('gtk' in option and 'window-close=off' in option for option in command):
                print('GPU passthrough: use Shutdown inside the guest to close the VM. '
                      'The window X button is disabled so AMDGPU can shut down first.', flush=True)
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
                                ended_utc=datetime.now(timezone.utc).isoformat(),
                                **gpu_exit.snapshot())
                sync_json(run / 'launch.json', metadata)
                if gpu_exit.initialized and not gpu_exit.shutdown_completed:
                    print('QEMU exited without an observed guest AMDGPU shutdown callback.',
                          file=sys.stderr, flush=True)
                process.stderr.close()
                return process.returncode if process.returncode >= 0 else 128 - process.returncode
            finally:
                reader.close()


if __name__ == '__main__':
    sys.exit(main())
