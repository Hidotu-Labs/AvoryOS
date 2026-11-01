#!/usr/bin/env python3
"""Exercise unprivileged shell-style reopening of pipe descriptors."""
import os
from boot_probe import run_probe

INIT = r'''
#include <fcntl.h>
#include <stdio.h>
#include <sys/reboot.h>
#include <sys/wait.h>
#include <unistd.h>
#include <errno.h>
static int check(const char *prefix) {
 int p[2]; char path[64], c;
 if (pipe(p)) return 1;
 snprintf(path, sizeof(path), "%s/%d", prefix, p[1]);
 int w = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
 if (w < 0) { perror("open writer"); return 2; }
 close(p[1]);
 if (write(w, "x", 1) != 1) return 3;
 close(w);
 if (read(p[0], &c, 1) != 1 || c != 'x' || read(p[0], &c, 1) != 0) return 4;
 close(p[0]);
 if (pipe(p)) return 5;
 snprintf(path, sizeof(path), "%s/%d", prefix, p[0]);
 int r = open(path, O_RDONLY);
 if (r < 0) { perror("open reader"); return 6; }
 close(p[0]);
 if (write(p[1], "y", 1) != 1 || read(r, &c, 1) != 1 || c != 'y') return 7;
 close(r); close(p[1]); return 0;
}
int main(void) {
 setbuf(stdout, NULL); puts("PROC_FD_PIPE_BOOT: START");
 pid_t p = fork();
 if (!p) { if (setgid(1000) || setuid(1000)) _exit(90); int a=check("/dev/fd"); if (a) _exit(a); _exit(check("/proc/self/fd")); }
 int status=-1; if (p < 0 || waitpid(p,&status,0)!=p || status) printf("PROC_FD_PIPE_BOOT: FAIL status=%d\n",status);
 else puts("PROC_FD_PIPE_BOOT: PASS");
 reboot(RB_POWER_OFF); for (;;) pause();
}
'''
run_probe('proc-fd-pipe-boot', INIT, 'PROC_FD_PIPE_BOOT: PASS', accel=os.environ.get('AVORY_TEST_ACCEL', 'tcg'), timeout=90)
