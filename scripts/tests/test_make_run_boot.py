#!/usr/bin/env python3
"""Boot the real make-run path repeatedly, protecting the user's disk."""
import os
from pathlib import Path
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[2]

WRAPPER = r'''#!/usr/bin/env python3
import os
from pathlib import Path
import subprocess
import sys
import time
args = sys.argv[1:]
updated = []
i = 0
while i < len(args):
    arg = args[i]
    if arg in ('-chardev', '-serial', '-display'):
        i += 2
        continue
    if arg == '-audiodev':
        updated.extend([arg, 'none,id=snd0'])
        i += 2
        continue
    if arg == '-drive' and 'file=disk.img,' in args[i + 1]:
        updated.extend([arg, args[i + 1].replace('file=disk.img,', 'file=' + os.environ['AVORY_RUN_PRIVATE_DISK'] + ',')])
        i += 2
        continue
    updated.append(arg)
    i += 1
serial = Path(os.environ['AVORY_RUN_TEST_SERIAL'])
serial.unlink(missing_ok=True)
display = 'egl-headless' if os.environ['AVORY_RUN_TEST_VIRGL'] == '1' else 'none'
updated.extend(['-snapshot', '-boot', 'd', '-display', display, '-monitor', 'none',
                '-serial', 'file:' + str(serial), '-no-reboot'])
process = subprocess.Popen([os.environ['AVORY_RUN_REAL_QEMU'], *updated])
passed = False
try:
    deadline = time.monotonic() + 80
    while process.poll() is None and time.monotonic() < deadline:
        text = serial.read_text(errors='replace') if serial.exists() else ''
        if 'No userspace init could be started' in text or 'ELF load failed: File not found: /sbin/init' in text:
            break
        if "AvoryOS (type 'install' for installer, or login)" in text:
            passed = True
            break
        time.sleep(.25)
finally:
    if process.poll() is None:
        process.terminate()
    process.wait(timeout=10)
if not passed:
    raise SystemExit('make run did not reach the real console login; see ' + str(serial))
print('MAKE_RUN_BOOT: PASS ' + str(serial), flush=True)
'''


def identity(path):
    st = path.stat()
    return st.st_dev, st.st_ino, st.st_size, st.st_mtime_ns, st.st_ctime_ns


def main():
    virgl = os.environ.get('AVORY_RUN_TEST_VIRGL', '1')
    if virgl not in ('0', '1'):
        raise ValueError('AVORY_RUN_TEST_VIRGL must be 0 or 1')
    source = ROOT/'disk.img'
    if not source.is_file():
        raise RuntimeError('This regression requires an already built desktop disk')
    original = identity(source)
    out = ROOT/'build/make-run-boot'
    out.mkdir(parents=True, exist_ok=True)
    plan = subprocess.check_output(['make', '-n', 'run', 'RUN_VIRGL=' + virgl], cwd=ROOT, text=True)
    (out/'plan.log').write_text(plan)
    for mutation in ('image-add-firmware.py', 'disk-build-cache.py', 'incremental-disk-apps.py',
                     'nouveau-firmware-stage.py', 'truncate -s'):
        if mutation in plan:
            raise RuntimeError('make run traverses a disk mutation: ' + mutation)
    private = out/'disk.img'
    subprocess.run(['cp', '--reflink=auto', '--sparse=always', source, private], check=True)
    wrappers = out/'bin'
    wrappers.mkdir(exist_ok=True)
    wrapper = wrappers/'qemu-system-x86_64'
    wrapper.write_text(WRAPPER)
    wrapper.chmod(0o755)
    env = dict(os.environ, PATH=str(wrappers) + ':' + os.environ['PATH'],
               AVORY_RUN_REAL_QEMU=shutil.which('qemu-system-x86_64'),
               AVORY_RUN_PRIVATE_DISK=str(private), AVORY_RUN_TEST_VIRGL=virgl)
    for boot in range(1, 4):
        env['AVORY_RUN_TEST_SERIAL'] = str(out/f'serial-{boot}.log')
        with (out/f'make-{boot}.log').open('w') as log:
            subprocess.run(['make', 'run', 'RUN_VIRGL=' + virgl], cwd=ROOT, env=env,
                           stdout=log, stderr=subprocess.STDOUT, timeout=180, check=True)
        if identity(source) != original:
            raise RuntimeError('make run changed the user disk')
        print(f'MAKE_RUN_BOOT: boot {boot} reached login; original disk unchanged', flush=True)
    print('MAKE_RUN_BOOT: PASS (3 boots; no disk copy/provisioning in make run)', flush=True)


if __name__ == '__main__':
    main()
