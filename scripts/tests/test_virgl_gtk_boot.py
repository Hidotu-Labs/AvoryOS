#!/usr/bin/env python3
"""Exercise native frontend input and visible virgl frames on an isolated display.

Requires host Weston development headers, a GL render node and weston-screenshooter.
AVORY_TEST_KERNEL and AVORY_TEST_ACCEL select the private VM kernel/accelerator.
Uses SDL like make run; --frontend=gtk reproduces the former GTK launcher.
"""
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time
ROOT = Path(__file__).resolve().parents[2]
out = ROOT/'build/virgl-gtk-host'
out.mkdir(parents=True, exist_ok=True)
module = out/'input.so'
flags = subprocess.check_output(['pkg-config','--cflags','--libs','libweston-15','wayland-server'], text=True).split()
subprocess.run(['cc','-shared','-fPIC',str(ROOT/'scripts/tests/virgl_weston_input.c'),'-o',str(module),*flags], check=True)
with tempfile.TemporaryDirectory(prefix='virgl-gtk-runtime-') as runtime:
    env = dict(os.environ, XDG_RUNTIME_DIR=runtime, AVORY_VIRGL_TEST_INPUT=runtime+'/input')
    with (out/'host.log').open('w') as log:
        compositor = subprocess.Popen(['weston','--backend=headless','--renderer=gl',
            '--width=1280','--height=900','--shell=kiosk-shell.so','--socket=virgl-test',
            '--idle-time=0','--modules='+str(module),'--log='+str(out/'weston.log')],
            env=env, stdout=log, stderr=log)
        try:
            for _ in range(100):
                if compositor.poll() is not None:
                    raise RuntimeError('host compositor failed; see '+str(out/'weston.log'))
                if Path(runtime,'virgl-test').exists():
                    break
                time.sleep(.1)
            else:
                raise RuntimeError('host compositor socket did not appear')
            subprocess.run([sys.executable,str(ROOT/'scripts/tests/test_virgl_mesa_boot.py'),
                            '--mouse','--gtk-wayland-display=virgl-test',*sys.argv[1:]],env=env,check=True)
        finally:
            compositor.terminate()
            compositor.wait(timeout=10)
