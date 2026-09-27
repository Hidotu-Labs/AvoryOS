// sys_io_register.c — Calls all per-module registration functions and
// initialises console_termios.  This replaces the monolithic
// syscall_register_io() that used to live in sys_io.c.
#include "sys_io_shared.h"
#include "../console/klog.h"
#include "syscall.h"
#include <stdint.h>

// Forward declarations — each module exposes its own register function.
void syscall_register_fd(void);
void syscall_register_ioctl(void);
void syscall_register_stat(void);
void syscall_register_fs(void);
void syscall_register_aio(void);
void syscall_register_random(void);
void syscall_register_xattr(void);

// syscall_register_io is the single entry-point called from syscall_init().
void syscall_register_io(void) {
    syscall_register_fd();
    syscall_register_ioctl();
    syscall_register_stat();
    syscall_register_fs();
    syscall_register_aio();
    syscall_register_random();
    syscall_register_xattr();

    // Initialise console termios defaults (previously at bottom of
    // syscall_register_io in sys_io.c).
    console_termios.c_lflag = 0x0000000b;
    console_termios.c_iflag = 0x00000100;
    console_termios.c_oflag = 0x00000005;
    console_termios.c_cflag = 0x000000bf;
    for (int i = 0; i < NCCS; i++)
        console_termios.c_cc[i] = 0;
    console_termios.c_cc[0] = 0x03; // VINTR  (Ctrl-C)
    console_termios.c_cc[1] = 0x1C; // VQUIT
    console_termios.c_cc[2] = 0x7F; // VERASE
    console_termios.c_cc[3] = 0x15; // VKILL
    console_termios.c_cc[4] = 0x04; // VEOF

    klog_puts("[OK] I/O syscalls registered.\n");
}
