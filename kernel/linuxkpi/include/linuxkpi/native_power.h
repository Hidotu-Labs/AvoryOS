#ifndef LINUXKPI_NATIVE_POWER_H
#define LINUXKPI_NATIVE_POWER_H
/* Emergency guest actions: no userspace helper or GPU callback recursion. */
void asc_emergency_poweroff(void) __attribute__((noreturn));
void asc_emergency_restart(void) __attribute__((noreturn));
#endif
