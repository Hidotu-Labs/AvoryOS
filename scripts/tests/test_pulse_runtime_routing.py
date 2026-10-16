#!/usr/bin/env python3
"""Verify the desktop routing with real libpulse and a private UNIX listener.

No host audio daemon or device is used. Requires libpulse development files.
"""
from pathlib import Path
import os
import socket
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
accounts = (ROOT / 'scripts/configure-accounts.sh').read_text()
routing = accounts.split('# rcS starts root', 1)[1].split('# 5. D-Bus', 1)[0]
routing = routing[routing.index('\nif '):]
client = r'''
#include <pulse/pulseaudio.h>
#include <stdio.h>
#include <unistd.h>
int main(void) {
    pa_mainloop *loop = pa_mainloop_new();
    pa_context *context = pa_context_new(pa_mainloop_get_api(loop), "routing-test");
    if (!context || pa_context_connect(context, NULL,
            PA_CONTEXT_NOAUTOSPAWN | PA_CONTEXT_NOFAIL, NULL) < 0) return 1;
    for (int i = 0; i < 100; i++) {
        pa_mainloop_iterate(loop, 0, NULL);
        usleep(1000);
    }
    pa_context_disconnect(context);
    pa_context_unref(context);
    pa_mainloop_free(loop);
    return 0;
}
'''

with tempfile.TemporaryDirectory(prefix='avory-pulse-route-') as temp:
    root = Path(temp)
    pulse = root / 'pipewire/pulse'
    pulse.mkdir(parents=True, mode=0o700)
    source = root / 'client.c'
    source.write_text(client)
    flags = subprocess.check_output(['pkg-config', '--cflags', '--libs', 'libpulse'],
                                    text=True).split()
    subprocess.run(['cc', '-Wall', '-Wextra', '-Werror', source,
                    '-o', root / 'client', *flags], check=True)
    env = {key: value for key, value in os.environ.items()
           if not key.startswith('PULSE_') and key not in ('DISPLAY', 'WAYLAND_DISPLAY')}
    env.update(HOME=temp, XDG_CONFIG_HOME=temp, XDG_RUNTIME_DIR=temp,
               PULSE_CLIENTCONFIG='/dev/null')
    with socket.socket(socket.AF_UNIX) as listener:
        listener.bind(str(pulse / 'native'))
        listener.listen(1)
        listener.settimeout(3)
        # Run the actual wrapper fragment, substituting only its fixed path.
        script = 'USER_ID=0\n' + routing.replace('/run/pipewire', str(root / 'pipewire'))
        script += '\ntest -z "${PULSE_SERVER+x}"\nexec "$1"\n'
        process = subprocess.Popen(['sh', '-ec', script, 'sh', str(root / 'client')],
                                   env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        try:
            connection, _ = listener.accept()
            with connection:
                connection.settimeout(3)
                assert connection.recv(4096), 'libpulse sent no authentication request'
            output, error = process.communicate(timeout=5)
            assert process.returncode == 0, (output, error)
        finally:
            if process.poll() is None:
                process.kill()
                process.wait()

print('PASS: real libpulse connects to the shared native socket without PULSE_SERVER')
