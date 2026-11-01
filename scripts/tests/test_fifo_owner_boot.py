#!/usr/bin/env python3
"""Check FIFO creation permissions for an unprivileged process on tmpfs."""
import os
from boot_probe import run_probe

INIT = r'''
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/reboot.h>
#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>
int main(void) {
  setbuf(stdout, NULL); puts("FIFO_OWNER_BOOT: START");
  if (mkdir("/tmp/fifo-test", 0777) || chmod("/tmp/fifo-test", 0777)) { perror("mkdir/chmod"); goto fail; }
  pid_t p = fork();
  if (!p) {
    if (setgid(1000) || setuid(1000)) _exit(1);
    umask(0077);
    if (mkfifo("/tmp/fifo-test/fifo", 0666)) _exit(2);
    struct stat st;
    if (stat("/tmp/fifo-test/fifo", &st) || st.st_uid != 1000 || st.st_gid != 1000 || (st.st_mode & 0777) != 0600) _exit(3);
    int fd = open("/tmp/fifo-test/fifo", O_RDWR | O_NONBLOCK);
    if (fd < 0) _exit(4);
    if (write(fd, "x", 1) != 1) _exit(5);
    char c = 0; if (read(fd, &c, 1) != 1 || c != 'x') _exit(6);
    close(fd); _exit(0);
  }
  int status = -1;
  if (p < 0 || waitpid(p, &status, 0) != p || status) { printf("FIFO_OWNER_BOOT: child status=%d\n", status); goto fail; }
  puts("FIFO_OWNER_BOOT: PASS"); reboot(RB_POWER_OFF); for (;;) pause();
fail:
  puts("FIFO_OWNER_BOOT: FAIL"); reboot(RB_POWER_OFF); for (;;) pause();
}
'''
run_probe('fifo-owner-boot', INIT, 'FIFO_OWNER_BOOT: PASS', accel=os.environ.get('AVORY_TEST_ACCEL', 'tcg'), timeout=90)
