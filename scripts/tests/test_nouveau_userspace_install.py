#!/usr/bin/env python3
"""The focused installer aligns Mesa/NVK without rebuilding the desktop tree."""
import os
import configparser
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'scripts/setup-arch.sh').read_text()
prefix = source[:source.index('# Bootstrap the target root:')]
with tempfile.TemporaryDirectory(prefix='avory-nvk-install-') as temp:
    directory = Path(temp)
    scripts = directory / 'scripts'
    scripts.mkdir()
    installer = scripts / 'setup-arch.sh'
    installer.write_text(prefix + '\nexit 99\n')
    configurations = directory / 'config/xorg'
    configurations.mkdir(parents=True)
    for name in ('10-nouveau.conf', 'avory-nouveau-xorg.sh', 'avory-xorg-profile.sh', '44-nouveau-env.sh', '20-output.conf'):
        (configurations/name).write_bytes((root/'config/xorg'/name).read_bytes())
    (directory / 'build/arch').mkdir(parents=True)
    (directory / 'build/arch/.pacman-sync').touch()
    command = directory / 'unshare'
    command.write_text('#!/bin/sh\nprintf "%s\\n" "$@" > "$INSTALL_LOG"\n')
    command.chmod(0o755)
    log = directory / 'transaction'
    environment = dict(os.environ, PATH=f'{directory}:{os.environ["PATH"]}', INSTALL_LOG=str(log))
    subprocess.run(['bash', str(installer), '--nouveau-userspace'], env=environment, check=True)
    rootfs = directory / 'build/arch/rootfs'
    hook = (root/'config/xorg/44-nouveau-env.sh').read_bytes()
    assert (rootfs/'etc/profile.d/avory-nouveau.sh').read_bytes() == hook
    assert (rootfs/'etc/X11/xinit/xinitrc.d/44-nouveau-env.sh').read_bytes() == hook
    assert (rootfs/'etc/sddm.conf.d/20-output.conf').read_bytes() == (root/'config/xorg/20-output.conf').read_bytes()
    greeter = configparser.ConfigParser()
    greeter.read(rootfs/'etc/sddm.conf.d/20-output.conf')
    assert dict(entry.split('=', 1) for entry in greeter['General']['GreeterEnvironment'].split(',')) == {
        'NOUVEAU_USE_ZINK': '1', 'LIBGL_KOPPER_DRI2': 'true'}
    assert 'QT_QUICK_BACKEND' not in greeter['General']['GreeterEnvironment']
    configuration = directory / 'active.conf'
    for selected, expected in (('NouveauPrimary', '1:true'), ('Native', ':')):
        configuration.write_text(f'Identifier "{selected}"\n')
        probe = subprocess.run(['sh', '-c', '. "$1"; printf "%s:%s" "${NOUVEAU_USE_ZINK-}" "${LIBGL_KOPPER_DRI2-}"',
                                'sh', str(rootfs/'etc/profile.d/avory-nouveau.sh')],
                               env={**environment, 'AVORY_XORG_CONFIG': str(configuration)},
                               capture_output=True, text=True, check=True)
        assert probe.stdout == expected, probe.stdout
    arguments = log.read_text().splitlines()
    assert '-S' in arguments and '--needed' in arguments and '-Sy' not in arguments
    for package in ('mesa', 'vulkan-nouveau', 'vulkan-radeon', 'vulkan-swrast',
                    'vulkan-mesa-implicit-layers', 'vulkan-icd-loader', 'vulkan-tools', 'mesa-utils'):
        assert package in arguments
    assert subprocess.run(['bash', str(installer), '--unknown'], env=environment,
                          capture_output=True).returncode == 2
assert 'pacman_run -S --needed mesa vulkan-nouveau' in source
print('PASS: focused NVK/Mesa transaction, cached repo reuse, no desktop bootstrap and default NVK install')
