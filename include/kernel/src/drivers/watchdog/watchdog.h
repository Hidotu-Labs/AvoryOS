#ifndef _DRIVERS_WATCHDOG_H
#define _DRIVERS_WATCHDOG_H

#include <stdint.h>
#include <stdbool.h>

#define WATCHDOG_IOCTL_BASE 'W'

struct watchdog_info {
    uint32_t options;          /* Options the card/driver supports */
    uint32_t firmware_version; /* Firmware version of the card */
    uint8_t  identity[32];     /* Identity of the board */
};

#define WDIOC_GETSUPPORT    0x80285700 /* _IOR('W', 0, struct watchdog_info) */
#define WDIOC_GETSTATUS     0x80045701 /* _IOR('W', 1, int) */
#define WDIOC_GETBOOTSTATUS 0x80045702 /* _IOR('W', 2, int) */
#define WDIOC_GETTEMP       0x80045703 /* _IOR('W', 3, int) */
#define WDIOC_SETOPTIONS    0x80045704 /* _IOR('W', 4, int) */
#define WDIOC_KEEPALIVE     0x80045705 /* _IOR('W', 5, int) */
#define WDIOC_SETTIMEOUT    0xc0045706 /* _IOWR('W', 6, int) */
#define WDIOC_GETTIMEOUT    0x80045707 /* _IOR('W', 7, int) */
#define WDIOC_SETPRETIMEOUT 0xc0045708 /* _IOWR('W', 8, int) */
#define WDIOC_GETPRETIMEOUT 0x80045709 /* _IOR('W', 9, int) */
#define WDIOC_GETTIMELEFT   0x8004570a /* _IOR('W', 10, int) */

#define WDIOF_UNKNOWN       -1  /* Unknown flag error */
#define WDIOS_UNKNOWN       -1  /* Unknown status error */

#define WDIOF_OVERHEAT      0x0001  /* Reset due to CPU overheat */
#define WDIOF_FANFAULT      0x0002  /* Fan failed */
#define WDIOF_EXTERN1       0x0004  /* External relay 1 */
#define WDIOF_EXTERN2       0x0008  /* External relay 2 */
#define WDIOF_POWERUNDER    0x0010  /* Power bad/power fault */
#define WDIOF_CARDRESET     0x0020  /* Card previously reset the CPU */
#define WDIOF_POWEROVER     0x0040  /* Power over voltage */
#define WDIOF_SETTIMEOUT    0x0080  /* Set timeout (in seconds) */
#define WDIOF_MAGICCLOSE    0x0100  /* Supports magic close char */
#define WDIOF_PRETIMEOUT    0x0200  /* Pretimeout (in seconds) */
#define WDIOF_KEEPALIVEPING 0x8000  /* Keep alive ping reply */

#define WDIOS_DISABLECARD   0x0001  /* Turn off the watchdog timer */
#define WDIOS_ENABLECARD    0x0002  /* Turn on the watchdog timer */
#define WDIOS_TEMPPANIC     0x0004  /* Temperature causes a panic */

void watchdog_init(void);
void watchdog_tick(void);

#endif /* _DRIVERS_WATCHDOG_H */
