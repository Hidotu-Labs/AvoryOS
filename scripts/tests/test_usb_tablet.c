/* Exercise the real HID -> mouse -> evdev path without privileged hardware.
 * cc -O2 -ffunction-sections -fdata-sections -Ikernel/src \
 *   -Ikernel/linuxkpi/include -Ikernel/limine-protocol/include \
 *   scripts/tests/test_usb_tablet.c -Wl,--gc-sections -o /tmp/test-usb-tablet
 */
#define USB_MOUSE_SELFTEST 1
#include "../../kernel/src/drivers/usb/usb_mouse.c"
#include "../../kernel/src/drivers/input/mouse.c"
#include "../../kernel/src/drivers/input/evdev.c"
extern int puts(const char *);
bool usb_verbose;
hal_irq_state_t hal_irq_save(void) { return 0; }
void hal_irq_restore(hal_irq_state_t flags) { (void)flags; }
void lockdiag_spin_begin(const void *p, uint64_t ip) { (void)p; (void)ip; }
void lockdiag_spin_end(const void *p) { (void)p; }
void klog_puts(const char *s) { (void)s; }
void klog_uint64(uint64_t n) { (void)n; }
void klog_hex32(uint32_t n) { (void)n; }
void klogf(const char *s, ...) { (void)s; }
uint64_t lapic_timer_get_ms(void) { return 0; }
uint64_t pit_get_ticks(void) { return 0; }
uint32_t fb_get_width(void) { return 1280; }
uint32_t fb_get_height(void) { return 800; }
void wait_queue_wake_all(wait_queue_t *q) { (void)q; }
void epoll_notify_event(struct vfs_node *n, uint32_t e) { (void)n; (void)e; }
void lockdiag_keyboard_event(uint16_t c, bool p) { (void)c; (void)p; }
void usb_debug_hexdump(const char *s, const void *p, uint32_t n, uint32_t m) {
  (void)s; (void)p; (void)n; (void)m;
}

#define CHECK(expr) do { if (!(expr)) { puts("FAIL: " #expr); return 1; } } while (0)
static bool has_bit(const uint8_t *bits, unsigned bit) {
  return (bits[bit / 8] & (1U << (bit % 8))) != 0;
}
int main(void) {
  usb_mouse_selftest();
  CHECK(usb_mouse_selftest_failures == 0);
  evdev_ready = true;
  mouse_evdev.type = EVDEV_MOUSE;
  spinlock_init(&mouse_evdev.lock);
  struct vfs_node node = { .device = &mouse_evdev };
  uint8_t bits[8];
  struct input_absinfo abs;
  CHECK(evdev_vfs_ioctl(&node, 0x80084520, (uintptr_t)bits) == 0);
  CHECK(has_bit(bits, EV_REL) && !has_bit(bits, EV_ABS));
  mouse_apply_report(5, -3, 0, 0, 0);
  CHECK(mouse_evdev.ring[0].type == EV_REL && mouse_evdev.ring[0].value == 5);
  mouse_evdev.head = mouse_evdev.tail = 0;
  mouse_configure_absolute(true);
  CHECK(evdev_vfs_ioctl(&node, 0x80084520, (uintptr_t)bits) == 0);
  CHECK(has_bit(bits, EV_ABS));
  CHECK(evdev_vfs_ioctl(&node, 0x80084522, (uintptr_t)bits) == 0);
  CHECK(!has_bit(bits, REL_X) && !has_bit(bits, REL_Y) && has_bit(bits, REL_WHEEL));
  CHECK(evdev_vfs_ioctl(&node, 0x80084523, (uintptr_t)bits) == 0);
  CHECK(has_bit(bits, ABS_X) && has_bit(bits, ABS_Y));
  CHECK(evdev_vfs_ioctl(&node, 0x80084509, (uintptr_t)bits) == 0 && has_bit(bits, 0));
  CHECK(evdev_vfs_ioctl(&node, EVIOCGABS(ABS_X), (uintptr_t)&abs) == 0 && abs.maximum == 32767);
  struct usb_mouse_state ms = {0};
  CHECK(hid_parse_mouse_layout(hid_test_touchpad_descriptor,
                              sizeof(hid_test_touchpad_descriptor), &ms.layout));
  /* First report positions the cursor immediately, including at the origin. */
  const uint8_t report[] = {1, 0xff, 0x7f, 0, 0};
  usb_mouse_decode(&ms, report, sizeof(report));
  CHECK(mouse_evdev.ring[0].type == EV_ABS && mouse_evdev.ring[0].code == ABS_X &&
        mouse_evdev.ring[0].value == 32767);
  CHECK(mouse_evdev.ring[1].type == EV_ABS && mouse_evdev.ring[1].value == 0);
  CHECK(mouse_evdev.ring[2].type == EV_KEY && mouse_evdev.ring[2].value == 1);
  CHECK(mouse_evdev.ring[3].type == EV_SYN);
  CHECK(mouse_get_state().x == 1279 && mouse_get_state().y == 0);
  CHECK(evdev_vfs_ioctl(&node, EVIOCGABS(ABS_X), (uintptr_t)&abs) == 0 && abs.value == 32767);
  uint32_t before = mouse_evdev.head;
  mouse_apply_report(300, -200, 0, 0, 1); /* Ignore competing PS/2 motion. */
  CHECK(mouse_evdev.head == before);
  mouse_apply_absolute_report(0, 32767, -1, 0, 0);
  CHECK(mouse_get_state().x == 0 && mouse_get_state().y == 799);
  CHECK(mouse_evdev.ring[before + 2].type == EV_REL &&
        mouse_evdev.ring[before + 2].code == REL_WHEEL);
  /* Nonzero and unsigned 16-bit HID ranges must normalize correctly. */
  struct hid_mouse_field field = { .logical_min = 100, .logical_max = 1100 };
  CHECK(hid_absolute_coord(600, &field) == 16383);
  CHECK(hid_absolute_coord(-1, &field) == 0);
  CHECK(hid_absolute_coord(2000, &field) == 32767);
  uint8_t descriptor[sizeof(hid_test_touchpad_descriptor)];
  memcpy(descriptor, hid_test_touchpad_descriptor, sizeof(descriptor));
  descriptor[42] = 0xff; /* 0..65535, not signed -1 */
  CHECK(hid_parse_mouse_layout(descriptor, sizeof(descriptor), &ms.layout));
  struct hid_mouse_delta delta;
  const uint8_t max_report[] = {0, 0xff, 0xff, 0xff, 0xff};
  CHECK(usb_mouse_parse_report(&ms, max_report, sizeof(max_report), &delta));
  CHECK(delta.absolute_x && delta.absolute_y && delta.x == 32767 && delta.y == 32767);
  mouse_configure_absolute(false);
  CHECK(!mouse_has_absolute_axes());
  puts("PASS: HID regression suite; tablet capabilities, first report, edges, buttons, wheel, ranges and relative-mouse isolation");
  return 0;
}
