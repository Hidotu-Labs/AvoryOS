#!/usr/bin/env python3
"""Check real console authentication after rcS on a private desktop disk."""
import shutil
import subprocess
from boot_probe import ROOT, run_probe
from test_firefox_responsiveness_boot import prepare_desktop_disk

DIAG = r'''
#define _GNU_SOURCE
#include <crypt.h>
#include <errno.h>
#include <pwd.h>
#include <shadow.h>
#include <stdio.h>
#include <string.h>
int main(void) {
    setbuf(stdout, NULL);
    FILE *f = fopen("/etc/passwd", "r");
    char line[512];
    puts("LOGIN_DIAG: passwd contents");
    if (f) { while (fgets(line, sizeof(line), f)) fputs(line, stdout); fclose(f); }
    errno = 0;
    struct passwd *pw = getpwnam("root");
    printf("LOGIN_DIAG: root=%s errno=%d\n", pw ? pw->pw_passwd : "MISSING", errno);
    errno = 0;
    struct spwd *sp = getspnam("root");
    printf("LOGIN_DIAG: shadow=%s errno=%d prefix=%.3s\n", sp ? "FOUND" : "MISSING", errno, sp ? sp->sp_pwdp : "");
    if (sp) {
        char *hash = crypt("root", sp->sp_pwdp);
        printf("LOGIN_DIAG: root_hash_valid=%d errno=%d\n", hash && !strcmp(hash, sp->sp_pwdp), errno);
    }
    return 0;
}
'''

INIT = r'''
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/reboot.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
static long long now(void) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000LL + t.tv_nsec / 1000000;
}
static int wait_text(int fd, const char *text) {
    char buffer[32768]; size_t used = 0; buffer[0] = 0;
    long long deadline = now() + 20000;
    while (now() < deadline) {
        int n = read(fd, buffer + used, sizeof(buffer) - used - 1);
        if (n > 0) {
            fwrite(buffer + used, 1, n, stdout);
            used += n; buffer[used] = 0;
            if (strstr(buffer, text)) return 1;
            if (used > sizeof(buffer) / 2) { memmove(buffer, buffer + used / 2, used - used / 2); used -= used / 2; }
        }
        usleep(10000);
    }
    printf("LOGIN_WAIT_TIMEOUT: %s\n", text); return 0;
}
static int session(const char *user, int uid) {
    int master = posix_openpt(O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (master < 0 || grantpt(master) || unlockpt(master)) { perror("pty"); return 0; }
    char slave[128]; snprintf(slave, sizeof(slave), "%s", ptsname(master));
    pid_t child = fork();
    if (!child) {
        setsid(); int terminal = open(slave, O_RDWR);
        ioctl(terminal, TIOCSCTTY, 0);
        dup2(terminal, 0); dup2(terminal, 1); dup2(terminal, 2);
        close(master); if (terminal > 2) close(terminal);
        execl("/bin/avory-login", "avory-login", NULL); _exit(127);
    }
    int ok = wait_text(master, "login: ");
    char input[256]; snprintf(input, sizeof(input), "%s\n", user);
    if (ok) write(master, input, strlen(input));
    if (ok && uid == 0) {
        ok = wait_text(master, "Password: ");
        if (ok) { write(master, "incorrect\n", 10); ok = wait_text(master, "Login incorrect"); }
        if (ok) ok = wait_text(master, "login: ");
        if (ok) { write(master, "root\n", 5); ok = wait_text(master, "Password: "); }
        if (ok) write(master, "root\n", 5);
    }
    usleep(1500000);
    snprintf(input, sizeof(input), "if [ \"$(id -u)\" = %d ]; then printf 'AUTH_'; printf '%s_OK\\n'; fi; exit\n", uid, user);
    if (ok) { write(master, input, strlen(input)); snprintf(input, sizeof(input), "AUTH_%s_OK", user); ok = wait_text(master, input); }
    close(master); /* Reboot cleans up the login loop after both sessions. */
    printf("LOGIN_SESSION: user=%s result=%s\n", user, ok ? "PASS" : "FAIL");
    return ok;
}
static int run(const char *program, const char *arg) {
    pid_t p = fork();
    if (!p) { execl(program, program, arg, NULL); perror(program); _exit(127); }
    int status; waitpid(p, &status, 0);
    return WIFEXITED(status) ? WEXITSTATUS(status) : 255;
}
int main(void) {
    setbuf(stdout, NULL); puts("CONSOLE_LOGIN_BOOT: START");
    mount("proc", "/proc", "proc", 0, NULL);
    mount("sysfs", "/sys", "sysfs", 0, NULL);
    mount("tmpfs", "/tmp", "tmpfs", 0, NULL);
    mount("tmpfs", "/run", "tmpfs", 0, NULL);
    mkdir("/dev/pts", 0755); mount("devpts", "/dev/pts", "devpts", 0, NULL);
    run("/bin/login-diag", NULL);
    run("/bin/sh", "/etc/init.d/rcS");
    run("/bin/login-diag", NULL);
    int root = session("root", 0), avory = session("avory", 1000);
    puts(root && avory ? "CONSOLE_LOGIN_BOOT: PASS" : "CONSOLE_LOGIN_BOOT: FAIL");
    puts("CONSOLE_LOGIN_BOOT: DONE");
    sync(); reboot(RB_POWER_OFF); for (;;) sleep(10);
}
'''


def provision_accounts(out):
    fixture = out/'account-rootfs'
    for directory in ('etc', 'usr/bin', 'usr/share'):
        (fixture/directory).mkdir(parents=True, exist_ok=True)
    # Reproduce the earlier missing-root state and the packaged read-only
    # sudoers; the real provisioning script must handle both, repeatedly.
    passwd = (ROOT/'build/arch/rootfs/etc/passwd').read_text()
    (fixture/'etc/passwd').write_text(''.join(row + '\n' for row in passwd.splitlines()
                                               if not row.startswith('root:')))
    shutil.copyfile(ROOT/'build/arch/rootfs/etc/group', fixture/'etc/group')
    sudoers = fixture/'etc/sudoers'
    if sudoers.exists():
        sudoers.chmod(0o640)
    shutil.copyfile(ROOT/'build/arch/rootfs/etc/sudoers', sudoers)
    sudoers.chmod(0o440)
    for _ in range(2):
        subprocess.run([ROOT/'scripts/configure-accounts.sh', fixture], check=True)
    assert all(len(row.split(':')) == 9 for row in (fixture/'etc/shadow').read_text().splitlines())
    assert (fixture/'etc/group').stat().st_mode & 0o777 == 0o644
    assert sudoers.stat().st_mode & 0o777 == 0o440
    accounts = {row.split(':')[0]: row.split(':')[1] for row in (fixture/'etc/passwd').read_text().splitlines()}
    assert accounts['avory'] == '' and accounts['root'] == 'x'
    return fixture


def main():
    out = ROOT/'build/console-login-boot'
    out.mkdir(parents=True, exist_ok=True)
    (out/'diag.c').write_text(DIAG)
    for source, output in ((out/'diag.c', out/'login-diag'),
                           (ROOT/'userland/avory-login.c', out/'avory-login')):
        subprocess.run([ROOT/'toolchain/x86_64-linux-glibc/bin/x86_64-linux-gcc', '-O2',
                        f'--sysroot={ROOT}/build/arch/rootfs', f'-Wl,-T,{ROOT}/config/userland-link.ld',
                        source, '-lcrypt', '-o', output], check=True)
    fixture = provision_accounts(out)
    def prepare(disk, output):
        prepare_desktop_disk(disk, output, [(out/'login-diag', '/bin/login-diag')])
        for name in ('passwd', 'shadow', 'group', 'sudoers'):
            subprocess.run(['debugfs', '-w', '-R', f'rm /etc/{name}', disk], capture_output=True, check=True)
            result = subprocess.run(['debugfs', '-w', '-R', f'write {fixture}/etc/{name} /etc/{name}', disk], capture_output=True, text=True, check=True)
            if 'Allocated inode:' not in result.stdout:
                raise RuntimeError(result.stdout + result.stderr)
        subprocess.run(['debugfs', '-w', '-R', 'rm /bin/avory-login', disk], capture_output=True, check=True)
        result = subprocess.run(['debugfs', '-w', '-R', f'write {out}/avory-login /bin/avory-login', disk], capture_output=True, text=True, check=True)
        if 'Allocated inode:' not in result.stdout:
            raise RuntimeError(result.stdout + result.stderr)
    result = run_probe('console-login-boot', INIT, 'CONSOLE_LOGIN_BOOT: DONE',
                       cpus=4, memory='4G', timeout=150, prepare_disk=prepare,
                       kernel_cmdline='kpi_amdgpu=0 init=/bin/firefox-probe-init',
                       qemu_args=['-netdev', 'user,id=net0', '-device', 'rtl8139,netdev=net0'])
    log = (result/'serial.log').read_text(errors='replace')
    if 'CONSOLE_LOGIN_BOOT: PASS' not in log:
        raise RuntimeError(f'Console login failed; see {result}/serial.log')

if __name__ == '__main__':
    main()
