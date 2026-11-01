#!/usr/bin/env python3
"""Exercise unprivileged /proc/self/comm renaming (Chromium/Electron thread naming)."""
import os
from boot_probe import run_probe

INIT = r'''
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/reboot.h>
#include <sys/wait.h>
#include <unistd.h>
static int check(void) {
 char buf[32] = {0};
 int fd = open("/proc/self/comm", O_WRONLY | O_CLOEXEC);
 if (fd < 0) { perror("open comm"); return 1; }
 if (write(fd, "electron-test\n", 14) != 14) return 2;
 close(fd);
 fd = open("/proc/self/comm", O_RDONLY | O_CLOEXEC);
 if (fd < 0) return 3;
 if (read(fd, buf, sizeof(buf) - 1) <= 0) return 4;
 close(fd);
 return strncmp(buf, "electron-test", 13) == 0 ? 0 : 5;
}
int main(void) {
 setbuf(stdout, NULL); puts("PROC_SELF_COMM_BOOT: START");
 pid_t p = fork();
 if (!p) { if (setgid(1000) || setuid(1000)) _exit(90); _exit(check()); }
 int status = -1;
 if (p < 0 || waitpid(p, &status, 0) != p || status) printf("PROC_SELF_COMM_BOOT: FAIL status=%d\n", status);
 else puts("PROC_SELF_COMM_BOOT: PASS");
 reboot(RB_POWER_OFF); for (;;) pause();
}
'''
run_probe('proc-self-comm-boot', INIT, 'PROC_SELF_COMM_BOOT: PASS', accel=os.environ.get('AVORY_TEST_ACCEL', 'tcg'), timeout=90)
