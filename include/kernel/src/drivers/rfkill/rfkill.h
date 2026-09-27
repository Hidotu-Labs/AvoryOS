#ifndef _DRIVERS_RFKILL_H
#define _DRIVERS_RFKILL_H

#include <stdbool.h>
#include <stdint.h>

/*
 * rfkill — Linux-compatible RF switch subsystem.
 *
 * Exposes /dev/rfkill with the same userspace ABI as the Linux kernel
 * (include/uapi/linux/rfkill.h) so tools like BlueZQt, util-linux' rfkill and
 * systemd-rfkill can control radio soft/hard blocks without modifications.
 *
 * Radios register themselves through rfkill_register(); with no radio drivers
 * in the tree there are simply no switches, which is exactly what Linux
 * reports on a machine without wireless hardware.
 */

enum rfkill_type {
  RFKILL_TYPE_ALL = 0,
  RFKILL_TYPE_WLAN,
  RFKILL_TYPE_BLUETOOTH,
  RFKILL_TYPE_UWB,
  RFKILL_TYPE_WIMAX,
  RFKILL_TYPE_WWAN,
  RFKILL_TYPE_GPS,
  RFKILL_TYPE_FM,
  RFKILL_TYPE_NFC,
  NUM_RFKILL_TYPES,
};

enum rfkill_operation {
  RFKILL_OP_ADD = 0,
  RFKILL_OP_DEL,
  RFKILL_OP_CHANGE,
  RFKILL_OP_CHANGE_ALL,
};

/* Wire format read from / written to /dev/rfkill (8 bytes, packed). */
struct rfkill_event {
  uint32_t idx;
  uint8_t type;
  uint8_t op;
  uint8_t soft;
  uint8_t hard;
} __attribute__((packed));

#define RFKILL_EVENT_SIZE_V1 8

/* ioctls — values match _IO('R', 1) and _IOW('R', 2, uint32_t) on x86_64. */
#define RFKILL_IOC_MAGIC 'R'
#define RFKILL_IOCTL_NOINPUT 0x5201u  /* _IO('R', 1)  */
#define RFKILL_IOCTL_MAX_SIZE 0x40045202u /* _IOW('R', 2, uint32_t) */

/* A radio switch (one 802.11 card, Bluetooth controller, ...). */
typedef struct rfkill_switch {
  uint32_t idx;       /* index reported to userspace */
  uint8_t type;       /* enum rfkill_type */
  bool soft_blocked;  /* software block state */
  bool hard_blocked;  /* hardware (kill switch) state */
  char name[48];      /* hardware name, e.g. "hci0" */
  /* Optional driver callback invoked when userspace changes the soft state.
   * Runs in process context with no rfkill lock held. */
  void (*set_block)(struct rfkill_switch *sw, bool blocked);
  void *data;
} rfkill_switch_t;

/* Register /dev/rfkill.  Must be called after /dev is mounted. */
void rfkill_init(void);

/* Driver API.  rfkill_register() returns NULL on failure. */
rfkill_switch_t *rfkill_register(uint8_t type, const char *name,
                                 void (*set_block)(rfkill_switch_t *, bool),
                                 void *data);
void rfkill_unregister(rfkill_switch_t *sw);

/* Report state changes observed in hardware (kill switch flipped, ...). */
void rfkill_set_sw_state(rfkill_switch_t *sw, bool blocked);
void rfkill_set_hw_state(rfkill_switch_t *sw, bool blocked);

/* Current state, for drivers that act on userspace requests. */
bool rfkill_soft_blocked(const rfkill_switch_t *sw);
bool rfkill_hard_blocked(const rfkill_switch_t *sw);

#endif /* _DRIVERS_RFKILL_H */
