/* Minimal PID 1 for the isolated NVMe test image. */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define COMMAND_FILE "/etc/nvme-test.command"
#define COMMAND_SIZE 4096
#define MAX_ARGS 64

int main(void) {
  char command[COMMAND_SIZE];
  FILE *file = fopen(COMMAND_FILE, "r");
  if (!file) {
    perror("nvme-init: open command file");
    return 127;
  }

  if (!fgets(command, sizeof(command), file)) {
    if (ferror(file))
      perror("nvme-init: read command file");
    fclose(file);
    return 127;
  }
  fclose(file);

  char *argv[MAX_ARGS] = {0};
  size_t argc = 0;
  char *save = NULL;
  for (char *arg = strtok_r(command, " \t\r\n", &save); arg;
       arg = strtok_r(NULL, " \t\r\n", &save)) {
    if (argc + 1 >= MAX_ARGS) {
      fputs("nvme-init: too many command arguments\n", stderr);
      return 127;
    }
    argv[argc++] = arg;
  }
  if (argc == 0) {
    fputs("nvme-init: empty command file\n", stderr);
    return 127;
  }

  execv(argv[0], argv);
  fprintf(stderr, "nvme-init: exec %s: %s\n", argv[0], strerror(errno));
  return 127;
}
