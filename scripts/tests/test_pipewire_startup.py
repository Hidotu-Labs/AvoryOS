#!/usr/bin/env python3
"""Check rcS/Plasma audio routing without starting host audio services."""
from pathlib import Path
import os
import socket
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
accounts = (ROOT / 'scripts/configure-accounts.sh').read_text()
setup = (ROOT / 'scripts/setup-arch.sh').read_text()
routing = accounts.split('# rcS starts root', 1)[1].split('# 5. D-Bus', 1)[0]
routing = routing[routing.index('\nif '):]
session = accounts.split('# 5b.', 1)[1].split('\nREAL_BIN=', 1)[0]
session = session[session.index('\nif '):]
bridge = setup.split('        ebegin "Starting PipeWire PulseAudio bridge"', 1)[1]
bridge = bridge.split('\n    fi\n    unset PIPE_RUNTIME', 1)[0]
bridge = bridge.replace('/var/log/pipewire.log', '$XDG_RUNTIME_DIR/pipewire.log')

with tempfile.TemporaryDirectory(prefix='avory-audio-') as temp:
    runtime = Path(temp)
    (runtime / 'pulse').mkdir()
    env = dict(os.environ, XDG_RUNTIME_DIR=temp, PIPE_RUNTIME=temp)
    env.pop('PIPEWIRE_RUNTIME_DIR', None)
    env.pop('PULSE_SERVER', None)
    env.pop('PULSE_RUNTIME_PATH', None)

    # Root must select the graph started by rcS and launch no second daemon.
    result = subprocess.run(['sh', '-ec', 'USER_ID=0\n' + routing + session +
                             '\nprintf "%s\\n" "$PIPEWIRE_RUNTIME_DIR" "$PULSE_RUNTIME_PATH" "${PULSE_SERVER-unset}"'],
                            env=env, capture_output=True, text=True, check=True)
    assert result.stdout.splitlines() == ['/run/pipewire', '/run/pipewire/pulse', 'unset']
    for inherited, expected in [('unix:/run/pipewire/pulse/native', 'unset'),
                                ('tcp:custom.example', 'tcp:custom.example')]:
        result = subprocess.run(['sh', '-ec', 'USER_ID=0\n' + routing +
                                 '\nprintf "%s\\n" "${PULSE_SERVER-unset}"'],
                                env=dict(env, PULSE_SERVER=inherited),
                                capture_output=True, text=True, check=True)
        assert result.stdout.strip() == expected
    result = subprocess.run(['sh', '-ec', 'USER_ID=1000\n' + routing +
                             '\nprintf "%s\\n" "${PULSE_RUNTIME_PATH-unset}" "${PULSE_SERVER-unset}"'],
                            env=env, capture_output=True, text=True, check=True)
    assert result.stdout.splitlines() == ['unset', 'unset']
    assert 'PIPE_RUNTIME=/run/pipewire' in setup

    # The bridge listens on pulse/native. An existing socket must suppress
    # another launch, and the boot success report must depend on that socket.
    with socket.socket(socket.AF_UNIX) as listener:
        listener.bind(str(runtime / 'pulse/native'))
        result = subprocess.run(['sh', '-ec', '''
pipewire-pulse() { echo unexpected-launch; }
eend() { echo status="$1"; }
eskip() { echo unexpected-skip; }
''' + bridge], env=env, capture_output=True, text=True, check=True)
        assert result.stdout.strip() == 'status=0', result.stdout
    (runtime / 'pulse/native').unlink()
    result = subprocess.run(['sh', '-ec', '''
pipewire-pulse() { :; }
nohup() { "$@"; }
sleep() { :; }
eend() { echo status="$1"; }
eskip() { echo unexpected-skip; }
''' + bridge], env=env, capture_output=True, text=True, check=True)
    assert result.stdout.strip() == 'status=1', result.stdout

for name, expected in [('pipewire-pulse', 'pipewire-pulse'),
                       ('pipewire-alsa', 'pipewire-alsa'),
                       ('pipewire-libs', 'libpipewire'),
                       ('pulseaudio-utils', 'libpulse')]:
    result = subprocess.check_output(['sh', '-c',
                                      '. "$1"; arch_name "$2"', 'sh',
                                      str(ROOT / 'scripts/arch-pkgs.sh'), name], text=True)
    assert result.strip() == expected, (name, result)

print('PASS: root uses rcS audio, bridge readiness is checked, package splits are preserved')
