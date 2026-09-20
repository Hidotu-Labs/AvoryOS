#ifndef DRIVERS_INPUT_MOUSE_H
#define DRIVERS_INPUT_MOUSE_H

#include <stdbool.h>
#include <stdint.h>

typedef struct {
  int32_t x;
  int32_t y;
  bool left_button;
  bool right_button;
  bool middle_button;
} mouse_state_t;

typedef struct {
  int32_t x;
  int32_t y;
  uint32_t buttons;
} mouse_device_packet_t;

void mouse_init(void);
mouse_state_t mouse_get_state(void);
void mouse_register_vfs(void);

/* Apply one relative pointer report from any mouse backend (PS/2 IRQ, USB HID).
 * `buttons` uses HID button order: bit0=left, bit1=right, bit2=middle,
 * bit3=side, bit4=extra.  Updates the global mouse state and pushes the
 * matching evdev events (REL_X/REL_Y/REL_WHEEL/REL_HWHEEL + EV_KEY + SYN). */
void mouse_apply_report(int32_t dx, int32_t dy, int32_t wheel, int32_t hwheel,
                        uint32_t buttons);

#endif