#!/usr/bin/env python3
"""Exercise SDDM's Avory login and Plasma Wayland on a private disk copy.

Uses KVM and offscreen SDL/virgl by default; AVORY_SDDM_VISIBLE=1 displays the
VM on the host Wayland session. The user's disk is only read when cloning it.
AVORY_SDDM_PASSWORDLESS=1 checks Breeze's passwordless user model and logs in
through the regular greeter instead of using autologin.
"""
import os
import re
import struct
import subprocess

from boot_probe import ROOT, run_probe
from test_firefox_responsiveness_boot import prepare_desktop_disk
from test_console_login_boot import provision_accounts

INIT = r'''

#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/reboot.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
static unsigned long long ms(void) {
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return t.tv_sec * 1000ULL + t.tv_nsec / 1000000;
}
static void dump(const char *path) {
  char b[4096];
  int f = open(path, O_RDONLY);
  if (f < 0)
    return;
  printf("LOG %s:\n", path);
  int n;
  while ((n = read(f, b, sizeof(b))) > 0)
    fwrite(b, 1, n, stdout);
  close(f);
  puts("\nEND LOG");
}
static int process(const char *name) {
  DIR *d = opendir("/proc");
  if (!d)
    return 0;
  struct dirent *e;
  int found = 0;
  while ((e = readdir(d))) {
    if (e->d_name[0] < '0' || e->d_name[0] > '9')
      continue;
    char p[128], b[64] = {0};
    snprintf(p, sizeof(p), "/proc/%s/comm", e->d_name);
    int f = open(p, O_RDONLY);
    if (f < 0)
      continue;
    read(f, b, sizeof(b) - 1);
    close(f);
    b[strcspn(b, "\n")] = 0;
    if (strcmp(b, name))
      continue;
    snprintf(p, sizeof(p), "/proc/%s/status", e->d_name);
    FILE *status = fopen(p, "r");
    unsigned uid = 0;
    if (status) {
      char line[256];
      while (fgets(line, sizeof(line), status))
        if (sscanf(line, "Uid:%u", &uid) == 1)
          break;
      fclose(status);
    }
    if (uid == 1000) {
      found = atoi(e->d_name);
      break;
    }
  }
  closedir(d);
  return found;
}
int main(void) {
  setbuf(stdout, NULL);
  puts("SDDM_AVORY_BOOT: START");
  mount("proc", "/proc", "proc", 0, NULL);
  mount("sysfs", "/sys", "sysfs", 0, NULL);
  mount("tmpfs", "/tmp", "tmpfs", 0, NULL);
  mount("tmpfs", "/run", "tmpfs", 0, NULL);
  int result =
      open("/root/sddm-probe-result.log", O_CREAT | O_TRUNC | O_WRONLY, 0644);
  dup2(result, 1);
  close(result);
  setenv("HOME", "/", 1);
  setenv("PATH", "/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin",
         1);
  pid_t services = fork();
  if (!services) {
    int log =
        open("/var/log/sddm-test-boot.log", O_CREAT | O_TRUNC | O_WRONLY, 0644);
    dup2(log, 1);
    dup2(log, 2);
    close(log);
    execl("/bin/sh", "sh", "/etc/init.d/rcS", NULL);
    _exit(127);
  }
  waitpid(services, NULL, 0);
  struct stat sys;
  stat("/sys", &sys);
  printf("SYSFS_ROOT: mode=%o uid=%u gid=%u\n", sys.st_mode & 07777, sys.st_uid,
         sys.st_gid);
  pid_t access_check = fork();
  if (!access_check) {
    int f = open("/var/log/sddm-test-access.log", O_CREAT | O_TRUNC | O_WRONLY,
                 0644);
    setgroups(0, NULL);
    setgid(1000);
    setuid(1000);
    errno = 0;
    int attr = open("/sys/class/drm/card0/device/vendor", O_RDONLY);
    dprintf(f, "SYSFS_USER: uid=%u vendor_fd=%d errno=%d\n", getuid(), attr,
            errno);
    if (attr >= 0)
      close(attr);
    close(f);
    _exit(0);
  }
  waitpid(access_check, NULL, 0);
  pid_t diag = fork();
  if (!diag) {
    int log = open("/var/log/sddm-test-devices.log",
                   O_CREAT | O_TRUNC | O_WRONLY, 0644);
    dup2(log, 1);
    dup2(log, 2);
    close(log);
    execl("/bin/sh", "sh", "-c",
          "ls -ln /dev/dri; id avory; cat /etc/sddm.conf.d/99-avory-test.conf",
          NULL);
    _exit(127);
  }
  waitpid(diag, NULL, 0);
  pid_t daemon = fork();
  if (!daemon) {
    int f = open("/var/log/sddm-probe.log", O_CREAT | O_TRUNC | O_WRONLY, 0644);
    dup2(f, 1);
    dup2(f, 2);
    close(f);
    execl("/usr/bin/sddm", "sddm", NULL);
    _exit(127);
  }
  unsigned long long start = ms();
  int stable = 0, first = 0, good = 1;
  while (ms() - start < 90000) {
    sleep(10);
    int shell = process("plasmashell"), kwin = process("kwin_wayland");
    printf("SDDM_AVORY_BOOT: SAMPLE second=%llu shell=%d kwin=%d\n",
           (ms() - start) / 1000, shell, kwin);
    if (shell && kwin) {
      if (!first)
        first = shell;
      if (first != shell)
        good = 0;
      stable++;
    } else if (stable)
      good = 0;
    int status;
    if (waitpid(daemon, &status, WNOHANG) == daemon) {
      printf("SDDM exited status=%d\n", status);
      good = 0;
      break;
    }
  }
  dump("/var/log/sddm-test-devices.log");
  dump("/var/log/sddm-test-access.log");
  dump("/var/log/sddm-probe.log");
  dump("/home/avory/.local/share/sddm/wayland-session.log");
  dump("/home/avory/desktop-ready");
  dump("/home/avory/screenshot.log");
  dump("/home/avory/dbus-check.log");
  printf("SDDM_AVORY_BOOT: %s stable_samples=%d\n",
         good && stable >= 5 && access("/home/avory/desktop-ready", F_OK) == 0
             ? "PASS"
             : "FAIL",
         stable);
  puts("SDDM_AVORY_BOOT: DONE");
  int kmsg = open("/dev/kmsg", O_WRONLY);
  if (kmsg >= 0) {
    dprintf(kmsg, "SDDM_AVORY_BOOT: DONE\n");
    close(kmsg);
  }
  sync();
  reboot(RB_POWER_OFF);
  for (;;)
    pause();
}
'''


def main():
    out = ROOT / 'build/sddm-avory-boot'
    out.mkdir(exist_ok=True)
    config = out/'99-avory-test.conf'
    passwordless = os.environ.get('AVORY_SDDM_PASSWORDLESS') == '1'
    config.write_text('[Autologin]\nUser=\nSession=\n[Theme]\nCurrent=breeze\n' if passwordless
                      else '[Autologin]\nUser=avory\nSession=plasma.desktop\nRelogin=false\n')
    fixture = provision_accounts(out) if passwordless else None
    if passwordless:
        state = out/'state.conf'
        state.write_text('[Last]\nUser=avory\nSession=/usr/share/wayland-sessions/plasma.desktop\n')
        theme = (ROOT/'build/arch/rootfs/usr/share/sddm/themes/breeze/Main.qml').read_text()
        # Exercise the real greeter's user model and its normal PAM login
        # interface, with a blank password and no autologin PAM bypass.
        theme = theme.replace('    id: root', '''    id: root
    property bool passwordlessAvory: false
    Repeater {
        model: userModel
        delegate: Item {
            required property string name
            required property bool needsPassword
            Component.onCompleted: {
                if (name === "avory" && !needsPassword)
                    root.passwordlessAvory = true
            }
        }
    }
    Timer {
        interval: 5000; running: true; repeat: false
        onTriggered: {
            if (!root.passwordlessAvory)
                return
            root.grabToImage(function(image) {
            console.log("PASSWORDLESS_SCREENSHOT", image.saveToFile("/var/lib/sddm/passwordless-greeter.png"))
            sddm.login("avory", "", sessionModel.lastIndex)
            })
        }
    }''', 1)
        (out/'greeter-Main.qml').write_text(theme)
    desktop = out/'avory-test.desktop'
    desktop.write_text('[Desktop Entry]\nType=Application\nName=Avory session regression\nExec=/bin/sh /usr/bin/avory-session-check\nOnlyShowIn=KDE;\n')
    check = out/'avory-session-check'
    check.write_text('''#!/bin/sh
{
 id
 printf 'HOME=%s\\nXDG_CONFIG_HOME=%s\\nXDG_CACHE_HOME=%s\\nWAYLAND_DISPLAY=%s\\n' "$HOME" "$XDG_CONFIG_HOME" "$XDG_CACHE_HOME" "$WAYLAND_DISPLAY"
 test "$(id -u)" = 1000 && test "$HOME" = /home/avory && test -w "$XDG_CONFIG_HOME" && test -w "$XDG_CACHE_HOME" && echo ACCOUNT_OK
} > /home/avory/desktop-ready
sleep 15
timeout 20 spectacle --background --nonotify --output /home/avory/sddm-test-desktop.png > /home/avory/screenshot.log 2>&1
timeout 10 qdbus6 org.kde.plasmashell /PlasmaShell org.freedesktop.DBus.Peer.Ping > /home/avory/dbus-check.log 2>&1 && echo DBUS_SHELL_OK >> /home/avory/desktop-ready
''')
    files = [(config, '/etc/sddm.conf.d/99-avory-test.conf'),
             (desktop, '/etc/xdg/autostart/avory-test.desktop'),
             (check, '/usr/bin/avory-session-check')]

    def prepare(disk, output):
        prepare_desktop_disk(disk, output, files)
        if passwordless:
            overlays = [(fixture/'etc'/name, '/etc/'+name) for name in ('passwd', 'shadow', 'group', 'sudoers')]
            overlays += [(out/'state.conf', '/var/lib/sddm/state.conf'),
                         (out/'greeter-Main.qml', '/usr/share/sddm/themes/breeze/Main.qml')]
            for source, target in overlays:
                subprocess.run(['debugfs', '-w', '-R', f'rm {target}', disk], capture_output=True, check=True)
                result = subprocess.run(['debugfs', '-w', '-R', f'write {source} {target}', disk], capture_output=True, text=True, check=True)
                if 'Allocated inode:' not in result.stdout:
                    raise RuntimeError(result.stdout + result.stderr)
            subprocess.run(['debugfs', '-w', '-R', 'rm /var/lib/sddm/passwordless-greeter.png', disk], capture_output=True, check=True)
        # A supplied filesystem may have old test artifacts. Require this
        # boot to produce its own readiness markers, log and screenshot.
        for path in ('/home/avory/.local/share/sddm/wayland-session.log',
                     '/home/avory/desktop-ready',
                     '/home/avory/sddm-test-desktop.png',
                     '/home/avory/screenshot.log', '/home/avory/dbus-check.log'):
            subprocess.run(['debugfs', '-w', '-R', f'rm {path}', disk],
                           capture_output=True, check=True)

    env = dict(os.environ, SDL_VIDEODRIVER='wayland' if os.environ.get('AVORY_SDDM_VISIBLE') else 'offscreen',
               LD_LIBRARY_PATH=str(ROOT/'build/virgl-host/lib')+':'+os.environ.get('LD_LIBRARY_PATH', ''))
    result = run_probe('sddm-avory-boot', INIT, 'SDDM_AVORY_BOOT: DONE',
                      cpus=8, memory='4G', timeout=180, prepare_disk=prepare,
                      gpu='virtio-vga-gl,xres=1280,yres=800', display='sdl,gl=on', qemu_env=env,
                      kernel_cmdline='kpi_amdgpu=0 init=/bin/firefox-probe-init',
                      qemu_args=['-device', 'qemu-xhci', '-device', 'usb-kbd', '-device', 'usb-mouse',
                                 '-netdev', 'user,id=net0', '-device', 'rtl8139,netdev=net0'])
    (result/'result.log').unlink(missing_ok=True)
    (result/'desktop.png').unlink(missing_ok=True)
    subprocess.run(['debugfs', '-R', f'dump /root/sddm-probe-result.log {result}/result.log', result/'root.img'], capture_output=True, check=True)
    log = (result/'result.log').read_text(errors='replace')
    print(log)
    subprocess.run(['debugfs', '-R', f'dump /home/avory/sddm-test-desktop.png {result}/desktop.png', result/'root.img'], capture_output=True, check=True)
    required = ('SDDM_AVORY_BOOT: PASS', 'ACCOUNT_OK', 'DBUS_SHELL_OK',
                'SYSFS_ROOT: mode=555')
    if (any(marker not in log for marker in required)
            or not re.search(r'SYSFS_USER: uid=1000 vendor_fd=[0-9]+ errno=0', log)
            or 'Display server stopped.' in log):
        raise RuntimeError(f'SDDM/Avory Plasma login failed; see {result}/result.log')
    if passwordless:
        if 'Authentication for user  "avory"  successful' not in log:
            raise RuntimeError('The greeter did not advertise and authenticate the blank-password account')
        (result/'greeter.png').unlink(missing_ok=True)
        subprocess.run(['debugfs', '-R', f'dump /var/lib/sddm/passwordless-greeter.png {result}/greeter.png', result/'root.img'], capture_output=True, check=True)
        if (result/'greeter.png').read_bytes()[:8] != b'\x89PNG\r\n\x1a\n':
            raise RuntimeError('Missing actual greeter screenshot')
    screenshot = (result/'desktop.png').read_bytes()
    if (screenshot[:8] != b'\x89PNG\r\n\x1a\n'
            or min(struct.unpack('>II', screenshot[16:24])) < 480):
        raise RuntimeError('Plasma did not capture a valid desktop screenshot')
    print('PASS: SDDM logged in as Avory; Plasma shell and KWin stayed running for at least 50 seconds.')


if __name__ == '__main__':
    main()
