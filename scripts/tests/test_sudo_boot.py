#!/usr/bin/env python3
"""Exercise sudo's real PAM password conversation on a private desktop disk."""
import subprocess
import re
from boot_probe import ROOT, run_probe
from test_console_login_boot import INIT, provision_accounts
from test_firefox_responsiveness_boot import prepare_desktop_disk

SUDO = r'''
static void child_signal(int sig) { (void)sig; }
static int sudo_session(void) {
    /* PID 1's default disposition auto-reaps idle children in this kernel.
     * A shell-style handler keeps the exit status available to waitpid. */
    signal(SIGCHLD, child_signal);
    int master = posix_openpt(O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (master < 0 || grantpt(master) || unlockpt(master)) return 0;
    char slave[128]; snprintf(slave, sizeof(slave), "%s", ptsname(master));
    pid_t child = fork();
    if (!child) {
        setsid(); int terminal = open(slave, O_RDWR);
        ioctl(terminal, TIOCSCTTY, 0);
        dup2(terminal, 0); dup2(terminal, 1); dup2(terminal, 2);
        close(master); if (terminal > 2) close(terminal);
        initgroups("avory", 1000);
        setgid(1000); setuid(1000);
        setenv("HOME", "/home/avory", 1);
        execl("/usr/bin/sudo", "sudo", "-k", "-p", "SUDO_PASSWORD:", "/usr/bin/id", "-u", NULL);
        _exit(127);
    }
    long long start = now();
    int ok = wait_text(master, "SUDO_PASSWORD:");
    printf("SUDO_PROMPT_MS=%lld\n", now() - start);
    if (ok) write(master, "root\n", 5);
    long long auth = now();
    int status = 0, exited = 0;
    while (now() - auth < 20000) {
        struct pollfd pfd = {.fd = master, .events = POLLIN};
        if (poll(&pfd, 1, 10) > 0 && (pfd.revents & POLLIN)) {
            char buffer[2048]; int n = read(master, buffer, sizeof(buffer));
            if (n > 0) fwrite(buffer, 1, n, stdout);
        }
        int waited = waitpid(child, &status, WNOHANG);
        if (waited == child) { exited = 1; break; }
        if (waited < 0) { printf("SUDO_WAIT_ERROR=%d child=%d\n", errno, child); break; }
        usleep(10000);
    }
    printf("SUDO_AUTH_MS=%lld exited=%d status=%d\n", now() - auth, exited, status);
    if (!exited) kill(child, SIGKILL);
    close(master);
    return ok && exited && WIFEXITED(status) && WEXITSTATUS(status) == 0;
}
static void dump_file(const char *path) {
    char buffer[4096]; int f = open(path, O_RDONLY), n;
    printf("DUMP %s\n", path);
    if (f >= 0) { while ((n = read(f, buffer, sizeof(buffer))) > 0) fwrite(buffer, 1, n, stdout); close(f); }
}
'''


def main():
    out = ROOT/'build/sudo-boot'
    out.mkdir(parents=True, exist_ok=True)
    fixture = provision_accounts(out)
    debug = out/'sudo.conf'
    debug.write_text('Debug sudo /tmp/sudo.debug all@debug\nDebug sudoers.so /tmp/sudo.debug all@debug\n')
    init = INIT.replace('#include <errno.h>', '#include <errno.h>\n#include <grp.h>\n#include <poll.h>')
    init = init.replace('int main(void) {', SUDO+'\nint main(void) {')
    init = init.replace('CONSOLE_LOGIN_BOOT', 'SUDO_BOOT')
    init = init.replace('    run("/bin/login-diag", NULL);', '')
    init = init.replace('mkdir("/dev/pts", 0755); mount("devpts", "/dev/pts", "devpts", 0, NULL);', '')
    init = init.replace('    int root = session("root", 0), avory = session("avory", 1000);',
                        '    dump_file("/proc/mounts");\n'
                        '    int root = sudo_session(), avory = 1;\n    dump_file("/tmp/sudo.debug");')

    def prepare(disk, output):
        prepare_desktop_disk(disk, output)
        for source, path in [(fixture/'etc'/name, '/etc/'+name) for name in ('passwd', 'shadow', 'group', 'sudoers')] + [(debug, '/etc/sudo.conf'), (ROOT/'config/init/rcS', '/etc/init.d/rcS')]:
            subprocess.run(['debugfs', '-w', '-R', f'rm {path}', disk], capture_output=True, check=True)
            result = subprocess.run(['debugfs', '-w', '-R', f'write {source} {path}', disk], capture_output=True, text=True, check=True)
            if 'Allocated inode:' not in result.stdout:
                raise RuntimeError(result.stdout + result.stderr)
        for command in ('set_inode_field /usr/bin/sudo uid 0',
                        'set_inode_field /usr/bin/sudo gid 0',
                        'set_inode_field /usr/bin/sudo mode 0104755'):
            subprocess.run(['debugfs', '-w', '-R', command, disk], capture_output=True, check=True)
        subprocess.run(['bash', ROOT/'scripts/configure-image-root.sh', disk,
                        ROOT/'build/arch/rootfs'], check=True)

    result = run_probe('sudo-boot', init, 'SUDO_BOOT: DONE', cpus=4, memory='4G', timeout=100,
                       prepare_disk=prepare, kernel_cmdline='kpi_amdgpu=0 init=/bin/firefox-probe-init',
                       qemu_args=['-netdev', 'user,id=net0', '-device', 'rtl8139,netdev=net0'])
    log = (result/'serial.log').read_text(errors='replace')
    if 'SUDO_BOOT: PASS' not in log:
        raise RuntimeError(f'Sudo failed; see {result}/serial.log')
    for name in ('SUDO_PROMPT_MS', 'SUDO_AUTH_MS'):
        elapsed = re.search(name+r'=(\d+)', log)
        if not elapsed or int(elapsed[1]) >= 3000:
            raise RuntimeError(f'Slow sudo {name}; see {result}/serial.log')


if __name__ == '__main__':
    main()
