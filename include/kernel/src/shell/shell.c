#include "shell/shell.h"

#include "console/console.h"
#include "drivers/input/keyboard.h"
#include "drivers/serial.h"
#include "drivers/usb/ehci.h"
#include "drivers/usb/ohci.h"
#include "drivers/usb/uhci.h"
#include "drivers/usb/usb_kbd.h"
#include "drivers/usb/xhci.h"
#include "fs/vfs.h"
#include "lib/string.h"
#include "sched/sched.h"

#include <stdint.h>

#define CMD_BUFFER_SIZE 256

static char command_buffer[CMD_BUFFER_SIZE];
static uint32_t command_length;

static void print_prompt(void) { console_puts("AscentOS> "); }

static char *skip_spaces(char *text) {
  while (*text == ' ' || *text == '\t')
    text++;
  return text;
}

static void command_echo(char *arguments) {
  console_puts(skip_spaces(arguments));
  console_putchar('\n');
}

static void command_ls(char *arguments) {
  char *path = skip_spaces(arguments);
  if (*path == '\0')
    path = "/";

  vfs_node_t *directory = vfs_resolve_path(path);
  if (!directory) {
    console_puts("ls: cannot access '");
    console_puts(path);
    console_puts("': No such file or directory\n");
    return;
  }

  if ((directory->flags & FS_TYPE_MASK) != FS_DIRECTORY) {
    console_puts("ls: '");
    console_puts(path);
    console_puts("': Not a directory\n");
    vfs_close(directory);
    return;
  }

  for (uint32_t index = 0;; index++) {
    struct dirent *entry = vfs_readdir(directory, index);
    if (!entry)
      break;
    console_puts(entry->name);
    console_putchar('\n');
  }

  vfs_close(directory);
}

static void print_uint64(uint64_t value) {
  char buffer[20];
  uint32_t length = 0;

  if (value == 0) {
    console_putchar('0');
    return;
  }

  while (value != 0) {
    buffer[length++] = (char)('0' + value % 10);
    value /= 10;
  }
  while (length != 0)
    console_putchar(buffer[--length]);
}

static void command_usb(void) {
  console_puts("USB controllers: xHCI=");
  print_uint64((uint64_t)xhci_get_controller_count());
  console_puts(" EHCI=");
  print_uint64((uint64_t)ehci_get_controller_count());
  console_puts(" OHCI=");
  print_uint64((uint64_t)ohci_get_controller_count());
  console_puts(" UHCI=");
  print_uint64((uint64_t)uhci_get_controller_count());
  console_putchar('\n');

  console_puts("USB keyboards: ");
  print_uint64(usb_kbd_active_count());
  console_putchar('\n');
  console_puts("[USB-KBD] HID report: ");
  console_puts(usb_kbd_report_format());
  console_putchar('\n');

  console_puts("xHCI matched: ");
  print_uint64((uint64_t)xhci_get_matched_count());
  console_puts(", last probe: ");
  console_puts(xhci_get_last_probe_failure());
  console_putchar('\n');

  for (int index = 0; index < xhci_get_controller_count(); index++) {
    struct xhci_controller *controller = xhci_get_controller(index);
    if (!controller)
      continue;

    console_puts("xHCI ");
    print_uint64((uint64_t)index);
    console_puts(": events=");
    print_uint64(controller->events_seen);
    console_puts(" interrupts=");
    print_uint64(controller->interrupts);
    console_puts(" transfers=");
    print_uint64(controller->hcd.stats.interrupt_completed);
    console_puts(" timeouts=");
    print_uint64(controller->timeouts);
    console_puts(" stage=");
    console_puts(controller->debug_stage ? controller->debug_stage : "unknown");
    console_putchar('\n');
  }
}

static void execute_command(char *command) {
  command = skip_spaces(command);
  char *end = command + strlen(command);
  while (end > command && (end[-1] == ' ' || end[-1] == '\t'))
    *--end = '\0';

  if (*command == '\0')
    return;

  if (strcmp(command, "echo") == 0) {
    command_echo(command + 4);
  } else if (strncmp(command, "echo ", 5) == 0 ||
             strncmp(command, "echo\t", 5) == 0) {
    command_echo(command + 4);
  } else if (strcmp(command, "ls") == 0) {
    command_ls(command + 2);
  } else if (strncmp(command, "ls ", 3) == 0 ||
             strncmp(command, "ls\t", 3) == 0) {
    command_ls(command + 2);
  } else if (strcmp(command, "usb") == 0) {
    command_usb();
  } else {
    console_puts("Unknown command\n");
  }
}

void shell_init(void) {
  command_length = 0;
  command_buffer[0] = '\0';
  console_puts("\nAscentOS kernel shell\n");
  command_usb();
}

void shell_run(void) {
  print_prompt();
  console_redraw_all();
  console_set_cursor_visible(true);

  for (;;) {
    console_refresh_cursor();

    char input = 0;
    if (keyboard_has_char())
      input = keyboard_get_char();
    else if (serial_received())
      input = serial_get_char();

    if (input == 0) {
      sched_yield();
      continue;
    }

    if (input == (char)KEY_UP) {
      console_scroll_view(1);
    } else if (input == (char)KEY_DOWN) {
      console_scroll_view(-1);
    } else if (input == (char)KEY_PGUP) {
      console_scroll_view(10);
    } else if (input == (char)KEY_PGDN) {
      console_scroll_view(-10);
    } else if (input == '\r' || input == '\n') {
      console_putchar('\n');
      command_buffer[command_length] = '\0';
      execute_command(command_buffer);
      command_length = 0;
      command_buffer[0] = '\0';
      print_prompt();
    } else if (input == '\b' || input == 0x7f) {
      if (command_length > 0) {
        command_buffer[--command_length] = '\0';
        console_putchar('\b');
      }
    } else if (input >= ' ' && input <= '~' &&
               command_length < CMD_BUFFER_SIZE - 1) {
      command_buffer[command_length++] = input;
      console_putchar(input);
    }
  }
}
