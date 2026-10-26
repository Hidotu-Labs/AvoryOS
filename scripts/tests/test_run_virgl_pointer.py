#!/usr/bin/env python3
"""Check the actual make run command's GL/pointer pairing and user overrides."""
from pathlib import Path
import os
import shlex
import subprocess

ROOT = Path(__file__).resolve().parents[2]

def launch(*overrides):
    environment = dict(os.environ)
    for variable in ('RUN_POINTER', 'RUN_VIRGL', 'RUN_VIRGL_HOST_FIX', 'RUN_DISPLAY', 'GTK_SHOW_CURSOR', 'DISPLAY_OPT', 'QEMUFLAGS'):
        environment.pop(variable, None)
    command = subprocess.check_output([
        'make', '--no-print-directory', '-n', '-o', 'edk2-ovmf',
        '-o', 'avoryos-x86_64.iso', '-o', 'disk.img', 'run', *overrides,
    ], cwd=ROOT, env=environment, text=True)
    tokens = shlex.split(command.replace('\\\n', ' '))
    guarded = 'RUN_VIRGL=0' not in overrides and 'RUN_VIRGL_HOST_FIX=0' not in overrides
    assert any(token.startswith('LD_LIBRARY_PATH='+str(ROOT/'build/virgl-host/lib')+':')
               for token in tokens) == guarded, command
    devices = [tokens[i+1] for i, token in enumerate(tokens) if token == '-device']
    display = tokens[tokens.index('-display')+1]
    return devices, display

devices, display = launch()
assert 'usb-mouse,bus=xhci.0' in devices, devices
assert any(device.startswith('virtio-vga-gl,') for device in devices), devices
assert 'gl=on' in display and 'show-cursor=off' in display, display
assert display.startswith('sdl,'), display
assert 'grab-on-hover' not in display and 'zoom-to-fit' not in display, display

devices, display = launch('RUN_VIRGL=0')
assert 'usb-mouse,bus=xhci.0' in devices, devices
assert any(device.startswith('virtio-vga,') for device in devices), devices
assert 'gl=off' in display and 'show-cursor=off' in display, display
assert display.startswith('gtk,') and 'grab-on-hover=on' in display, display

devices, display = launch('RUN_POINTER=usb-tablet')
assert 'usb-tablet,bus=xhci.0' in devices, devices
assert 'gl=on' in display and 'show-cursor=off' in display, display

_, display = launch('GTK_SHOW_CURSOR=off')
assert 'show-cursor=off' in display, display
_, display = launch('RUN_POINTER=usb-mouse', 'GTK_SHOW_CURSOR=on')
assert 'show-cursor=on' in display, display
_, display = launch('DISPLAY_OPT=-display sdl,gl=on')
assert display == 'sdl,gl=on', display
_, display = launch('RUN_DISPLAY=gtk', 'RUN_VIRGL=1')
assert display.startswith('gtk,') and 'gl=on' in display and 'grab-on-hover=on' in display, display
devices, display = launch('RUN_VIRGL_HOST_FIX=0')
assert any(device.startswith('virtio-vga-gl,') for device in devices) and 'gl=on' in display
print('PASS: SDL virgl relative mouse, GTK 2D fallback, tablet, cursor and display overrides')
