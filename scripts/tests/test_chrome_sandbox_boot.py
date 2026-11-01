#!/usr/bin/env python3
"""Run Chromium's setuid sandbox helper (chrome-sandbox) as an unprivileged user.

Set AVORY_CHROME_SANDBOX to a chrome-sandbox binary (for example from an
Electron install) and AVORY_GUEST_ROOT to a guest root partition image. Make
the partition image with `dd if=disk.img of=root.img bs=1M skip=1`, since
disk.img is a GPT disk whose root partition starts at 1 MiB. The
helper is glibc-linked, so its loader and libc come from the guest, not the
host: the host's newer x86-64-v3 glibc traps with SIGILL under this kernel."""
import os
import shutil
import subprocess
from pathlib import Path
from boot_probe import run_probe, ROOT

SANDBOX = os.environ.get('AVORY_CHROME_SANDBOX')
GUEST_ROOT = os.environ.get('AVORY_GUEST_ROOT')

TARGET = r'''
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>
int main(void) {
 printf("SANDBOX_TARGET uid=%u euid=%u pid=%d ppid=%d\n", getuid(), geteuid(), getpid(), getppid());
 int fd = open("/proc/self/exe", O_RDONLY);
 printf("SANDBOX_TARGET proc/self/exe fd=%d errno=%d\n", fd, fd < 0 ? errno : 0);
 return 0;
}
'''

INIT = r'''
#include <fcntl.h>
#include <stdio.h>
#include <sys/reboot.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
int main(void) {
 setbuf(stdout, NULL); puts("CHROME_SANDBOX_BOOT: START");
 pid_t p = fork();
 if (!p) {
  if (setgid(1000) || setuid(1000)) _exit(90);
  /* The helper closes fd 7 unconditionally; Chromium's launcher leaves it open. */
  int nul = open("/dev/null", O_RDWR);
  if (nul < 0 || (nul != 7 && dup2(nul, 7) != 7)) _exit(92);
  char *argv[] = {"chrome-sandbox", "/bin/sandbox-target", NULL};
  char *envp[] = {"PATH=/bin", "SBX_CHROME_API_RQ=1", NULL};
  execve("/bin/chrome-sandbox", argv, envp);
  _exit(91);
 }
 int status = -1;
 if (p < 0 || waitpid(p, &status, 0) != p || status) printf("CHROME_SANDBOX_BOOT: FAIL status=%d\n", status);
 else puts("CHROME_SANDBOX_BOOT: PASS");
 reboot(RB_POWER_OFF); for (;;) pause();
}
'''

def prepare_disk(disk: Path, out: Path):
    # The image is built by a non-root user, so set root ownership and the
    # setuid bit on the helper with debugfs, as an installed Electron has.
    cmds = out / 'chrome-sandbox.debugfs'
    cmds.write_text('sif /bin/chrome-sandbox uid 0\nsif /bin/chrome-sandbox gid 0\n'
                    'sif /bin/chrome-sandbox mode 0104755\n')
    subprocess.run(['debugfs', '-w', '-f', str(cmds), str(disk)], check=True)

def setup(root: Path):
    shutil.copy2(SANDBOX, root / 'bin/chrome-sandbox')
    os.chmod(root / 'bin/chrome-sandbox', 0o755)
    (root / 'lib64').mkdir(exist_ok=True)
    (root / 'lib').mkdir(exist_ok=True)
    for src, dst in [('/usr/lib/ld-linux-x86-64.so.2', 'lib64/ld-linux-x86-64.so.2'),
                     ('/usr/lib/libc.so.6', 'lib/libc.so.6'),
                     ('/usr/lib/libc.so.6', 'usr/lib/libc.so.6')]:
        subprocess.run(['debugfs', '-R', f'dump {src} {root / dst}', GUEST_ROOT],
                       check=True, capture_output=True)
    target_c = root.parent / 'sandbox-target.c'
    target_c.write_text(TARGET)
    subprocess.run(['x86_64-linux-musl-gcc', '-O2', '-static', str(target_c),
                    '-o', str(root / 'bin/sandbox-target')], check=True)

if SANDBOX is None or GUEST_ROOT is None:
    print('CHROME_SANDBOX_BOOT: SKIP (set AVORY_CHROME_SANDBOX and AVORY_GUEST_ROOT)')
else:
    run_probe('chrome-sandbox-boot', INIT, 'CHROME_SANDBOX_BOOT: PASS', setup=setup, prepare_disk=prepare_disk,
              accel=os.environ.get('AVORY_TEST_ACCEL', 'tcg'), timeout=120)
