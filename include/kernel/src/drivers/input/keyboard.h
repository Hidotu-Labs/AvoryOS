#ifndef KEYBOARD_H
#define KEYBOARD_H

#include "../../sched/wait.h"
#include "drivers/input/scancode.h"
#include <stdbool.h>
#include <stdint.h>

extern wait_queue_t keyboard_wait_queue;

void keyboard_init(void);

// Returns true if there is a character in the keyboard buffer.
bool keyboard_has_char(void);

void keyboard_push_bytes(const char *bytes, uint32_t len);

// Halts execution until a character is typed, then returns it.
char keyboard_get_char(void);

// Scancode mode functions
bool keyboard_has_scancode(void);
bool keyboard_get_scancode(scancode_event_t *event);
void keyboard_set_scancode_mode(bool enabled);
bool keyboard_is_scancode_mode(void);
void keyboard_push_scancode(uint8_t scancode, bool extended, bool release);

#define KEY_F1 0x81
#define KEY_F2 0x82
#define KEY_F3 0x83
#define KEY_F4 0x84
#define KEY_F5 0x85
#define KEY_F6 0x86
#define KEY_F7 0x87
#define KEY_F8 0x88
#define KEY_F9 0x89
#define KEY_F10 0x8A
#define KEY_F11 0x8B
#define KEY_F12 0x8C

#define KEY_UP 0xE0
#define KEY_DOWN 0xE1
#define KEY_LEFT 0xE2
#define KEY_RIGHT 0xE3
#define KEY_PGUP 0xE4
#define KEY_PGDN 0xE5
#define KEY_HOME 0xE6
#define KEY_END 0xE7

#endif
