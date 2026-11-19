"""Verify legacy tmpfs mount options used by systemd-user-runtime-dir."""

from boot_probe import run_probe


INIT = r"""
#define _GNU_SOURCE
#include <fcntl.h>
#include <stdio.h>
#include <sys/mount.h>
#include <sys/reboot.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

int main(void) {
    struct stat st;
    if (mount("tmpfs", "/mnt", "tmpfs", MS_NOSUID | MS_NODEV,
              "mode=0700,uid=1000,gid=1000,size=64M") != 0 ||
        stat("/mnt", &st) != 0 || st.st_uid != 1000 || st.st_gid != 1000 ||
        (st.st_mode & 07777) != 0700)
        goto poweroff;

    pid_t child = fork();
    if (child == 0) {
        if (setgid(1000) != 0 || setuid(1000) != 0 ||
            open("/mnt/session", O_CREAT | O_WRONLY, 0600) < 0)
            _exit(1);
        _exit(0);
    }
    int status;
    if (child > 0 && waitpid(child, &status, 0) == child &&
        WIFEXITED(status) && WEXITSTATUS(status) == 0)
        puts("TMPFS_RUNTIME_OWNER: PASS");

poweroff:
    fflush(stdout);
    reboot(RB_POWER_OFF);
    return 1;
}
"""


if __name__ == '__main__':
    run_probe('tmpfs-runtime-owner', INIT, 'TMPFS_RUNTIME_OWNER: PASS',
              setup=lambda root: (root / 'mnt').mkdir(), accel='tcg')
