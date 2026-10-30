#!/usr/bin/env python3
"""Verify statfs/statvfs resolve relative paths from the process cwd."""
from boot_probe import run_probe


INIT = r'''
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/reboot.h>
#include <sys/statvfs.h>
#include <sys/stat.h>
#include <unistd.h>

static void finish(int ok, const char *why) {
  if (ok) puts("STATFS_CWD_BOOT: PASS");
  else printf("STATFS_CWD_BOOT: FAIL %s errno=%d\n", why, errno);
  reboot(RB_POWER_OFF);
  for (;;) pause();
}

int main(void) {
  setbuf(stdout, NULL);
  puts("STATFS_CWD_BOOT: START");
  if (chdir("/tmp")) finish(0, "chdir /tmp");
  if (mkdir("statfs-cwd", 0700) && errno != EEXIST)
    finish(0, "mkdir");
  if (chdir("statfs-cwd")) finish(0, "chdir relative directory");
  int fd = open("models.img", O_CREAT | O_TRUNC | O_RDWR, 0600);
  if (fd < 0) finish(0, "create relative file");
  close(fd);
  struct statvfs info;
  if (statvfs("models.img", &info)) finish(0, "relative statvfs");
  if (!info.f_bsize || !info.f_blocks) finish(0, "invalid filesystem stats");
  finish(1, "");
}
'''


if __name__ == '__main__':
    run_probe('statfs-cwd-boot', INIT, 'STATFS_CWD_BOOT: PASS',
              accel='tcg', cpus=1)
