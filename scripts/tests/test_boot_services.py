#!/usr/bin/env python3
"""Check rcS services on a private desktop filesystem, using real D-Bus calls."""
import subprocess
from boot_probe import ROOT, run_probe
from test_console_login_boot import INIT, provision_accounts
from test_firefox_responsiveness_boot import prepare_desktop_disk


def main():
    out = ROOT/'build/boot-services'
    out.mkdir(parents=True, exist_ok=True)
    fixture = provision_accounts(out)
    check = out/'service-check'
    check.write_text('''#!/bin/sh
owner() {
 dbus-send --system --print-reply --dest=org.freedesktop.DBus /org/freedesktop/DBus org.freedesktop.DBus.NameHasOwner "string:$1" | grep -q 'boolean true'
}
owner org.freedesktop.DBus && echo SERVICE_DBUS_OK
owner org.freedesktop.resolve1 && echo SERVICE_RESOLVED_OK
owner org.freedesktop.PolicyKit1 && echo SERVICE_POLKIT_OK
nmcli --wait 2 -t -f RUNNING general | grep -qx running && echo SERVICE_NM_OK
for name in dbus systemd-resolved polkit NetworkManager; do
 echo "SERVICE_LOG $name"
 cat "/var/log/$name.log" 2>/dev/null
done
''')
    init = INIT.replace('CONSOLE_LOGIN_BOOT', 'BOOT_SERVICES')
    init = init.replace('    run("/bin/login-diag", NULL);', '')
    init = init.replace('    run("/bin/sh", "/etc/init.d/rcS");',
                        '    setenv("SYSTEMD_LOG_LEVEL", "debug", 1); run("/bin/sh", "/etc/init.d/rcS");')
    init = init.replace('mkdir("/dev/pts", 0755); mount("devpts", "/dev/pts", "devpts", 0, NULL);', '')
    init = init.replace('    int root = session("root", 0), avory = session("avory", 1000);',
                        '    run("/bin/sh", "/bin/service-check");\n    int root = 1, avory = 1;')

    def prepare(disk, output):
        prepare_desktop_disk(disk, output)
        files = [(fixture/'etc'/name, '/etc/'+name) for name in ('passwd', 'shadow', 'group')]
        files += [(ROOT/'config/init/rcS', '/etc/init.d/rcS'), (check, '/bin/service-check')]
        for source, target in files:
            subprocess.run(['debugfs', '-w', '-R', f'rm {target}', disk], capture_output=True, check=True)
            result = subprocess.run(['debugfs', '-w', '-R', f'write {source} {target}', disk], capture_output=True, text=True, check=True)
            if 'Allocated inode:' not in result.stdout:
                raise RuntimeError(result.stdout + result.stderr)

    result = run_probe('boot-services', init, 'BOOT_SERVICES: DONE', cpus=4, memory='4G', timeout=120,
                       prepare_disk=prepare, kernel_cmdline='kpi_amdgpu=0 init=/bin/firefox-probe-init',
                       qemu_args=['-netdev', 'user,id=net0', '-device', 'rtl8139,netdev=net0'])
    log = (result/'serial.log').read_text(errors='replace')
    for service in ('DBUS', 'RESOLVED', 'POLKIT', 'NM'):
        if f'SERVICE_{service}_OK' not in log:
            raise RuntimeError(f'{service} failed; see {result}/serial.log')
    if '[ !! ]' in log:
        raise RuntimeError(f'Boot reported a failed service; see {result}/serial.log')
    print('PASS: D-Bus, resolved, polkit and NetworkManager respond; rcS reports no failed services.')


if __name__ == '__main__':
    main()
