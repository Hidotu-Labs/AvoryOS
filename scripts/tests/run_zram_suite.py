#!/usr/bin/env python3
"""Build and stress zram without rebuilding or opening the desktop disk image."""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--skip-build', action='store_true')
p.add_argument('--memory', default='512M')
a = p.parse_args()
out = ROOT / 'build/zram-suite'
out.mkdir(parents=True, exist_ok=True)

def run(name, command, env=None, boot_directory=None):
    print(f'Running {name}...', flush=True)
    with (out / (name + '.log')).open('w') as log:
        result = subprocess.run(command, cwd=ROOT, env=env, stdout=log, stderr=subprocess.STDOUT)
    if boot_directory:
        serial = ROOT / 'build' / boot_directory / 'serial.log'
        if serial.is_file():
            shutil.copy2(serial, out / (name + '-serial.log'))
    if result.returncode:
        print((out / (name + '.log')).read_text(errors='replace')[-5000:])
        raise SystemExit(f'{name} failed; see {out}')
    print(f'PASS {name}', flush=True)

if not a.skip_build:
    run('kernel-build', ['make', '-C', 'kernel', '-j' + str(min(os.cpu_count() or 2, 8))])
run('host-sanitizers', [sys.executable, 'scripts/tests/test_zram_host.py'])
run('fork-regression', [sys.executable, 'scripts/tests/test_lazy_fork.py'])
run('teardown-regression', [sys.executable, 'scripts/tests/test_vmm_deferred_shutdown.py'],
    env=dict(os.environ, ASAN_OPTIONS='detect_leaks=0'))
for cpus in (1, 4, 8):
    run(str(cpus) + '-cpu', [sys.executable, 'scripts/tests/test_zram_boot.py',
                           '--cpus', str(cpus), '--memory', a.memory], boot_directory='zram-boot')
for flag in ('disabled', 'limit'):
    run(flag, [sys.executable, 'scripts/tests/test_zram_boot.py', '--' + flag,
               '--memory', a.memory], boot_directory='zram-boot-off' if flag == 'disabled' else 'zram-boot-limit')
print(f'All zram stress tests passed. Logs: {out}')
