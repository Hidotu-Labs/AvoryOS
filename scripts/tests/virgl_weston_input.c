/* Virtual pointer and capture authority for the isolated test compositor.
 * These backend input entry points are exported by libweston, but are not
 * part of its installed public header. This helper targets libweston 15. */
#include <libweston/libweston.h>
#include <wayland-server-core.h>
#include <linux/input-event-codes.h>
#include <time.h>
#include <stdio.h>
#include <stdlib.h>

extern void weston_seat_init(struct weston_seat *, struct weston_compositor *, const char *);
extern int weston_seat_init_pointer(struct weston_seat *);
extern int weston_seat_init_keyboard(struct weston_seat *, struct xkb_keymap *);
extern void notify_motion(struct weston_seat *, const struct timespec *, struct weston_pointer_motion_event *);
extern void notify_button(struct weston_seat *, const struct timespec *, int32_t, enum wl_pointer_button_state);
extern void notify_pointer_frame(struct weston_seat *);

static struct weston_seat seat;
static struct wl_listener authority;
static struct wl_event_source *timer;
static int last = -1;
static bool down;
static double last_x = 640;

static void authorize(struct wl_listener *listener, struct weston_output_capture_attempt *attempt) {
    (void)listener;
    /* This module is loaded only into the private headless test compositor. */
    attempt->authorized = true;
}

static int tick(void *data) {
    (void)data;
    int index = -1;
    const char *path = getenv("AVORY_VIRGL_TEST_INPUT");
    FILE *file = path ? fopen(path, "r") : NULL;
    if (file) {
        if (fscanf(file, "%d", &index) != 1)
            index = -1;
        fclose(file);
    }
    struct timespec time;
    clock_gettime(CLOCK_MONOTONIC, &time);
    if (down) {
        notify_button(&seat, &time, BTN_LEFT, WL_POINTER_BUTTON_STATE_RELEASED);
        down = false;
    }
    if (index >= 0 && index != last) {
        last = index;
        double x = index % 2 ? 450 : 750;
        struct weston_pointer_motion_event event = {
            /* Include relative deltas so GTK's pointer-lock protocol sees
             * real motion when testing the relative USB mouse launcher. */
            .mask = WESTON_POINTER_MOTION_ABS | WESTON_POINTER_MOTION_REL |
                    WESTON_POINTER_MOTION_REL_UNACCEL,
            .time = time,
            .abs = {.c = {.x = x, .y = 500}},
            .rel = {.x = x - last_x, .y = 0},
            .rel_unaccel = {.x = x - last_x, .y = 0},
        };
        last_x = x;
        notify_motion(&seat, &time, &event);
        /* GTK's relative mode acquires its pointer grab on the first click;
         * grab-on-hover only acquires the keyboard. Click before the first
         * guest movement checkpoint, then keep exercising button delivery. */
        if (index == 0 || index >= 30) {
            notify_button(&seat, &time, BTN_LEFT, WL_POINTER_BUTTON_STATE_PRESSED);
            down = true;
        }
    }
    notify_pointer_frame(&seat);
    wl_event_source_timer_update(timer, 50);
    return 0;
}

__attribute__((visibility("default")))
int wet_module_init(struct weston_compositor *compositor, int *argc, char **argv) {
    (void)argc;
    (void)argv;
    weston_seat_init(&seat, compositor, "virgl-test-seat");
    if (weston_seat_init_pointer(&seat))
        return -1;
    /* Relative GTK grabs cover the entire seat. A pointer-only test seat
     * cannot reproduce the normal host keyboard/pointer grab behavior. */
    if (weston_seat_init_keyboard(&seat, NULL))
        return -1;
    weston_compositor_add_screenshot_authority(compositor, &authority, authorize);
    timer = wl_event_loop_add_timer(wl_display_get_event_loop(compositor->wl_display), tick, NULL);
    if (!timer)
        return -1;
    wl_event_source_timer_update(timer, 50);
    return 0;
}
