#include <linux/string.h>
#include <linux/types.h>

#include <linuxkpi/native.h>

bool linuxkpi_vkms_enabled(void) {
  const char *cmdline = linuxkpi_boot_cmdline();
  const char option[] = "boot_selftest=1";

  while (*cmdline) {
    while (*cmdline && strchr(" \t\r\n", *cmdline))
      cmdline++;

    size_t length = 0;
    while (cmdline[length] && !strchr(" \t\r\n", cmdline[length]))
      length++;

    if (length == sizeof(option) - 1 && !memcmp(cmdline, option, length))
      return true;

    cmdline += length;
  }

  return false;
}