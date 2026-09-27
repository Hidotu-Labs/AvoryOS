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

#endif
