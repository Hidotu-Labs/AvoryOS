#!/usr/bin/env python3
"""Exercise automatic RandR provider binding with actual xrandr-style output."""
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / 'config/xorg/avory-prime-display.sh'


def check(providers, expected, fail_bind=False, missing_output=False):
    with tempfile.TemporaryDirectory(prefix='avory-prime-startup-') as tmp:
        directory = Path(tmp)
        (directory / 'providers').write_text(providers)
        (directory / 'outputs').write_text('Screen 0: minimum 320 x 200, current 1280 x 800\nHDMI-1 connected 1024x768+0+0\nDP-1 disconnected\n' + ('' if missing_output else 'HDMI-1-1 connected\n'))
        command = directory / 'xrandr'
        command.write_text('''#!/bin/sh
printf '%s\\n' "$*" >> "$PRIME_TEST_DIR/calls"
case "$1" in
 --listproviders) cat "$PRIME_TEST_DIR/providers" ;;
 --setprovideroutputsource) [ "$PRIME_TEST_FAIL" != 1 ] ;;
 --query) cat "$PRIME_TEST_DIR/outputs" ;;
 --output|--auto) exit 0 ;;
 *) exit 99 ;;
esac
''')
        command.chmod(0o755)
        env = dict(os.environ, PATH=str(directory) + ':' + os.environ['PATH'],
                   PRIME_TEST_DIR=tmp, PRIME_TEST_FAIL=str(int(fail_bind)))
        result = subprocess.run(['sh', str(SCRIPT)], env=env,
                                capture_output=True, text=True)
        assert bool(result.returncode) == (fail_bind or missing_output), result.stderr
        calls = (directory / 'calls').read_text().splitlines()
        assert calls == expected, calls


amd = 'Provider 0: id: 0x43 cap: 0x9, Source Output, Sink Offload crtcs: 4 outputs: 4 associated providers: 0 name:modesetting\n'
virtio = 'Provider 1: id: 0x77 cap: 0x2, Sink Output crtcs: 1 outputs: 1 associated providers: 0 name:modesetting\n'
check('Providers: number : 2\n' + amd + virtio,
      ['--listproviders', '--setprovideroutputsource 0x77 0x43', '--query',
       '--output HDMI-1 --off --output DP-1 --off --output HDMI-1-1 --auto --primary --set PRIME Synchronization 0'])
check('Providers: number : 2\n' + virtio + amd,
      ['--listproviders', '--setprovideroutputsource 0x77 0x43', '--query',
       '--output HDMI-1 --off --output DP-1 --off --output HDMI-1-1 --auto --primary --set PRIME Synchronization 0'])
check('Providers: number : 1\n' + amd, ['--listproviders', '--auto'])
check('Providers: number : 1\nProvider 0: id: 0x43 cap: 0xf, Source Output, Sink Output, Source Offload, Sink Offload\n',
      ['--listproviders', '--auto'])
check('Providers: number : 2\n' + amd + virtio,
      ['--listproviders', '--setprovideroutputsource 0x77 0x43'], fail_bind=True)
check('Providers: number : 2\n' + amd + virtio,
      ['--listproviders', '--setprovideroutputsource 0x77 0x43', '--query'], missing_output=True)
print('PASS: PRIME direction, reordered IDs, single GPU, self-binding avoidance, output selection, unsynchronized presentation, and error propagation')
