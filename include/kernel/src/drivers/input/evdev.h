#ifndef DRIVERS_INPUT_EVDEV_H
#define DRIVERS_INPUT_EVDEV_H

#include "../../fs/vfs.h"
#include "../../lock/spinlock.h"
#include "../../sched/wait.h"
#include <stdbool.h>
#include <stdint.h>

// Linux input_event structure (ABI-compatible)
// Xorg's evdev driver reads these directly from /dev/input/eventN
struct input_event {
  uint64_t time_sec;  // struct timeval.tv_sec
  uint64_t time_usec; // struct timeval.tv_usec
  uint16_t type;
  uint16_t code;
  int32_t value;
};

// Event types (from linux/input-event-codes.h)
#define EV_SYN 0x00
#define EV_KEY 0x01
#define EV_REL 0x02
#define EV_ABS 0x03
#define EV_MSC 0x04
#define EV_SW 0x05
#define EV_LED 0x11
#define EV_REP 0x14
#define EV_MAX 0x1f

// Synchronization codes
#define SYN_REPORT 0
#define SYN_CONFIG 1
#define SYN_MT_REPORT 2
#define SYN_DROPPED 3

// Relative axis codes (EV_REL)
#define REL_X 0x00
#define REL_Y 0x01
#define REL_Z 0x02
#define REL_RX 0x03
#define REL_RY 0x04
#define REL_RZ 0x05
#define REL_HWHEEL 0x06
#define REL_DIAL 0x07
#define REL_WHEEL 0x08
#define REL_MISC 0x09
#define REL_MAX 0x0f

// Absolute axis codes (EV_ABS)
#define ABS_X 0x00
#define ABS_Y 0x01
#define ABS_MAX 0x3f

// Button / key codes (EV_KEY)
// Mouse buttons
#define BTN_MISC 0x100
#define BTN_LEFT 0x110
#define BTN_RIGHT 0x111
#define BTN_MIDDLE 0x112
#define BTN_SIDE 0x113
#define BTN_EXTRA 0x114
#define BTN_MOUSE 0x110

// Keyboard key codes (Linux evdev keycodes — NOT PS/2 scancodes)
// These are the same values used by X11's evdev driver.
// The table maps 1:1 from PS/2 set-1 scancodes for the basic range.
#define KEY_RESERVED 0
#define KEY_ESC 1
#define KEY_1 2
#define KEY_2 3
#define KEY_3 4
#define KEY_4 5
#define KEY_5 6
#define KEY_6 7
#define KEY_7 8
#define KEY_8 9
#define KEY_9 10
#define KEY_0 11
#define KEY_MINUS_EV 12
#define KEY_EQUAL 13
#define KEY_BACKSPACE_EV 14
#define KEY_TAB_EV 15
#define KEY_Q_EV 16
#define KEY_W_EV 17
#define KEY_E_EV 18
#define KEY_R_EV 19
#define KEY_T_EV 20
#define KEY_Y_EV 21
#define KEY_U_EV 22
#define KEY_I_EV 23
#define KEY_O_EV 24
#define KEY_P_EV 25
#define KEY_LEFTBRACE 26
#define KEY_RIGHTBRACE 27
#define KEY_ENTER_EV 28
#define KEY_LEFTCTRL 29
#define KEY_A_EV 30
#define KEY_S_EV 31
#define KEY_D_EV 32
#define KEY_F_EV 33
#define KEY_G_EV 34
#define KEY_H_EV 35
#define KEY_J_EV 36
#define KEY_K_EV 37
#define KEY_L_EV 38
#define KEY_SEMICOLON_EV 39
#define KEY_APOSTROPHE_EV 40
#define KEY_GRAVE_EV 41
#define KEY_LEFTSHIFT 42
#define KEY_BACKSLASH_EV 43
#define KEY_Z_EV 44
#define KEY_X_EV 45
#define KEY_C_EV 46
#define KEY_V_EV 47
#define KEY_B_EV 48
#define KEY_N_EV 49
#define KEY_M_EV 50
#define KEY_COMMA_EV 51
#define KEY_DOT_EV 52
#define KEY_SLASH_EV 53
#define KEY_RIGHTSHIFT 54
#define KEY_KPASTERISK_EV 55
#define KEY_LEFTALT 56
#define KEY_SPACE_EV 57
#define KEY_CAPSLOCK_EV 58
#define KEY_F1_EV 59
#define KEY_F2_EV 60
#define KEY_F3_EV 61
#define KEY_F4_EV 62
#define KEY_F5_EV 63
#define KEY_F6_EV 64
#define KEY_F7_EV 65
#define KEY_F8_EV 66
#define KEY_F9_EV 67
#define KEY_F10_EV 68
#define KEY_NUMLOCK_EV 69
#define KEY_SCROLLLOCK_EV 70
#define KEY_KP7_EV 71
#define KEY_KP8_EV 72
#define KEY_KP9_EV 73
#define KEY_KPMINUS_EV 74
#define KEY_KP4_EV 75
#define KEY_KP5_EV 76
#define KEY_KP6_EV 77
#define KEY_KPPLUS_EV 78
#define KEY_KP1_EV 79
#define KEY_KP2_EV 80
#define KEY_KP3_EV 81
#define KEY_KP0_EV 82
#define KEY_KPDOT_EV 83
#define KEY_F11_EV 87
#define KEY_F12_EV 88

// Extended keys (from E0-prefixed scancodes)
#define KEY_KPENTER_EV 96
#define KEY_RIGHTCTRL 97
#define KEY_KPSLASH_EV 98
#define KEY_RIGHTALT 100
#define KEY_HOME_EV 102
#define KEY_UP_EV 103
#define KEY_PAGEUP_EV 104
#define KEY_LEFT_EV 105
#define KEY_RIGHT_EV 106
#define KEY_END_EV 107
#define KEY_DOWN_EV 108
#define KEY_PAGEDOWN_EV 109
#define KEY_INSERT_EV 110
#define KEY_DELETE_EV 111
#define KEY_MAX_EV 0x2ff

// ioctl commands (from linux/input.h)
#define EVIOCGVERSION 0x80044501                     // get driver version
#define EVIOCGID 0x80084502                          // get device ID
#define EVIOCGNAME(len) (0x80004506 | ((len) << 16)) // get device name
#define EVIOCGPHYS(len) (0x80004507 | ((len) << 16)) // get physical location
#define EVIOCGUNIQ(len) (0x80004508 | ((len) << 16)) // get unique identifier
#define EVIOCGPROP(len) (0x80004509 | ((len) << 16)) // get device properties
#define EVIOCGBIT(ev, len)                                                     \
  (0x80004520 | ((ev) << 8) | ((len) << 16)) // get event bits
#define EVIOCGABS(abs) (0x80184540 | (abs))  // get abs info
#define EVIOCGRAB 0x40044590                 // grab/release device
#define EVIOCREVOKE 0x40044591               // revoke access to device

// Linux input_id structure
struct input_id {
  uint16_t bustype;
  uint16_t vendor;
  uint16_t product;
  uint16_t version;
};

// Linux input_absinfo structure
struct input_absinfo {
  int32_t value;
  int32_t minimum;
  int32_t maximum;
  int32_t fuzz;
  int32_t flat;
  int32_t resolution;
};

// Bus types
#define BUS_I8042 0x11

// Evdev device types
#define EVDEV_KEYBOARD 0
#define EVDEV_MOUSE 1

// Ring buffer size
#define EVDEV_RING_SIZE 256

// Evdev device structure
typedef struct evdev_device {
  int type;           // EVDEV_KEYBOARD or EVDEV_MOUSE
  char name[64];      // Device name string
  char phys[64];      // Physical path string
  struct input_id id; // Device identification

  struct input_event ring[EVDEV_RING_SIZE];
  volatile uint32_t head;
  volatile uint32_t tail;
  uint8_t key_state[(KEY_MAX_EV + 8) / 8];

  wait_queue_t wait;    // For blocking reads / poll
  spinlock_t lock;      // Protects head/tail and ring buffer
  vfs_node_t *vfs_node; // The /dev/input/eventN node
} evdev_device_t;

// Public API
void evdev_init(void);

// Push an event into a device's ring buffer (called from IRQ handlers)
void evdev_push_event(evdev_device_t *dev, uint16_t type, uint16_t code,
                      int32_t value);

// Stateful Linux EV_KEY reporting. Duplicate presses become value 2 repeats;
// duplicate releases are discarded. Call evdev_sync() once per input packet.
bool evdev_report_key(evdev_device_t *dev, uint16_t code, bool pressed);
void evdev_sync(evdev_device_t *dev);

// Get device handles so IRQ callbacks can push events
evdev_device_t *evdev_get_keyboard(void);
evdev_device_t *evdev_get_mouse(void);

// Convert a PS/2 set-1 scancode to a Linux evdev keycode
uint16_t evdev_ps2_to_keycode(uint8_t scancode, bool is_extended);

#endif
