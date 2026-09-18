/* Framebuffer damage-path benchmark.
 *
 * Enabled with `fb_bench=1` on the kernel command line (see KERNEL_CMDLINE in
 * the top-level GNUmakefile).  It runs once during early boot, right after the
 * console exists and before the first boot message, so the numbers measure
 * only the dirty-rectangle and swap path.  Results go to the serial console.
 *
 * Each case paints into the backbuffer, marks the kind of damage the console
 * produces, and times fb_swap_buffer() on the TSC.  The run ends by blanking
 * both buffers and clearing the damage state, so boot output is unaffected. */

#include "framebuffer.h"
#include "fb.h"
#include "../console/klog.h"
#include "../lib/string.h"
#include "../lib/tsc.h"

extern struct fb_info fb_global;
extern const char *kernel_boot_cmdline;

#define BENCH_FONT_H 16

static uint32_t b_w;
static uint32_t b_h;
static uint32_t b_pitch;

static void bench_fill_pattern(void) {
    uint32_t *bb = (uint32_t *)fb_global.backbuffer;
    uint32_t pitch_px = b_pitch / 4;

    for (uint32_t y = 0; y < b_h; y++) {
        uint32_t *row = bb + (uint64_t)y * pitch_px;
        for (uint32_t x = 0; x < b_w; x++)
            row[x] = (x * 2654435761u) ^ (y * 40503u);
    }
}

static void damage_full(uint32_t frame) {
    (void)frame;
    fb_mark_dirty(0, 0, b_w, b_h);
}

static void damage_full_scrolled(uint32_t frame) {
    (void)frame;
    /* The console scrolls by rotating the ring; the swap then splits the
     * full-screen copy at the wrap. */
    fb_backbuffer_scroll(BENCH_FONT_H);
    fb_mark_dirty(0, 0, b_w, b_h);
}

static void damage_text_line(uint32_t frame) {
    uint32_t line = b_h / (2 * BENCH_FONT_H);
    (void)frame;
    /* One printed line: 16 scanlines sharing a half-screen span. */
    fb_mark_dirty(0, line * BENCH_FONT_H, b_w / 2, BENCH_FONT_H);
}

static void damage_glyph(uint32_t frame) {
    (void)frame;
    /* A single character cell: the smallest interactive update. */
    fb_mark_dirty((b_w / 2) & ~7u, (b_h / 2) & ~15u, 8, BENCH_FONT_H);
}

static void damage_text_screen(uint32_t frame) {
    uint32_t lines = b_h / BENCH_FONT_H;

    if (lines > 25)
        lines = 25;
    (void)frame;
    /* A kilo-style repaint: every text row damaged once. */
    for (uint32_t y = 0; y < lines; y++)
        fb_mark_dirty(0, y * BENCH_FONT_H, b_w / 2, BENCH_FONT_H);
}

static void bench_report(const char *name, uint32_t frames,
                         uint64_t bytes_per_frame, uint64_t cycles) {
    uint64_t ns = tsc_cycles_to_ns(cycles);
    uint64_t bytes = bytes_per_frame * frames;

    klog_puts("[FB-BENCH] ");
    klog_puts(name);
    klog_puts(": ");

    if (ns && frames) {
        uint64_t per_frame_us = ns / frames / 1000ULL;
        /* bytes * 1000 / ns is MB/s without overflowing 64 bits for the
         * frame sizes here. */
        uint64_t mb_per_s = bytes * 1000ULL / ns;

        klog_uint64(per_frame_us);
        klog_puts(" us/frame, ");
        klog_uint64(mb_per_s);
        klog_puts(" MB/s copied");
    } else {
        /* TSC not calibrated: report raw cycles so the numbers still compare
         * against each other. */
        klog_uint64(cycles);
        klog_puts(" cycles");
    }
    klog_puts("\n");
}

static void bench_run(const char *name, uint32_t frames,
                      uint64_t bytes_per_frame, void (*damage)(uint32_t)) {
    uint64_t t0 = rdtsc_fence();

    for (uint32_t i = 0; i < frames; i++) {
        damage(i);
        fb_swap_buffer();
    }

    bench_report(name, frames, bytes_per_frame, rdtsc_fence() - t0);
}

void fb_bench_maybe_run(void) {
    if (!kernel_boot_cmdline || !strstr(kernel_boot_cmdline, "fb_bench"))
        return;

    fb_set_backbuffer_mode(true);
    if (!fb_is_backbuffer_enabled() || !fb_global.backbuffer) {
        klog_puts("[FB-BENCH] skipped: no backbuffer available\n");
        return;
    }

    b_w = fb_global.var.xres;
    b_h = fb_global.var.yres;
    b_pitch = fb_global.fix.line_length;

    klog_puts("[FB-BENCH] framebuffer damage benchmark: ");
    klog_uint64(b_w);
    klog_puts("x");
    klog_uint64(b_h);
    klog_puts(" pitch=");
    klog_uint64(b_pitch);
    klog_puts("\n");

    bench_fill_pattern();

    /* Frame counts keep the whole run under a couple of seconds while still
     * averaging out timer noise on the small cases. */
    bench_run("full-frame   ", 16, (uint64_t)b_pitch * b_h, damage_full);
    bench_run("full-scrolled", 16, (uint64_t)b_pitch * b_h,
              damage_full_scrolled);
    bench_run("text-line    ", 400, 16ULL * (b_w / 2) * 4, damage_text_line);
    bench_run("glyph        ", 4000, 16ULL * 8 * 4, damage_glyph);
    bench_run("text-screen  ", 64, 25ULL * 16 * (b_w / 2) * 4,
              damage_text_screen);

    /* Leave a blank screen and an empty damage table behind. */
    fb_clear(0);
    fb_swap_buffer();
    fb_set_backbuffer_mode(false);
}
