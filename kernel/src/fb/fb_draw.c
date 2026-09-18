#include "framebuffer.h"
#include "../lib/string.h"

extern struct fb_info fb_global;
extern void *fb_get_target_buffer(void);

/* ── Per-scanline dirty tracking ─────────────────────────────────────────────
 * The console paints horizontal runs: a prompt row, a status bar, a blinking
 * cursor.  A single bounding box coalesces those into a full-screen
 * frontbuffer blit, so keep the union of the damage on every scanline and let
 * the swap stitch full-width runs back into one copy.
 *
 * Two extra fast paths keep marking cheap:
 *  - a mark that covers the whole screen sets fb_dirty_full and never touches
 *    the table at all (fb_clear, scroll redraw, blank/unblank);
 *  - consecutive rows carrying the same span are coalesced by the swap into
 *    one blit per ring segment, so a glyph (16 scanlines) or a whole redrawn
 *    text line costs the copy engine a single call. */
#define FB_DIRTY_MAX_ROWS 8192
struct fb_dirty_row {
    uint32_t xmin; /* inclusive */
    uint32_t xmax; /* exclusive; the row is clean when xmin >= xmax */
};
static struct fb_dirty_row fb_dirty_rows[FB_DIRTY_MAX_ROWS]
    __attribute__((aligned(8)));
static uint32_t fb_dirty_min_row = FB_DIRTY_MAX_ROWS; /* inclusive scan bound */
static uint32_t fb_dirty_max_row = 0;                 /* exclusive scan bound */
static bool fb_dirty_full = false;                    /* table cannot describe it */

/* Physical backbuffer scanline backing display row @y.  The console scrolls
 * by rotating fb_global.backbuffer_origin one line at a time; the swap undoes
 * the rotation while it blits.  Only the backbuffer rotates - screen_base and
 * every other target stay linear. */
static inline uint32_t fb_row_phys(const void *target, uint32_t y) {
    if (target == fb_global.backbuffer && fb_global.backbuffer_origin) {
        uint32_t r = y + fb_global.backbuffer_origin;
        if (r >= fb_global.var.yres)
            r -= fb_global.var.yres;
        return r;
    }
    return y;
}

uint32_t fb_backbuffer_row(uint32_t y) {
    return fb_row_phys(fb_global.backbuffer, y);
}

void fb_backbuffer_scroll(uint32_t pixel_rows) {
    if (!fb_global.backbuffer || !fb_global.var.yres || !pixel_rows)
        return;

    uint32_t rows = pixel_rows % fb_global.var.yres;
    uint32_t origin = fb_global.backbuffer_origin + rows;
    if (origin >= fb_global.var.yres)
        origin -= fb_global.var.yres;
    fb_global.backbuffer_origin = origin;
}

static void fb_dirty_clear(uint32_t min_row, uint32_t max_row) {
    if (max_row >= FB_DIRTY_MAX_ROWS)
        max_row = FB_DIRTY_MAX_ROWS - 1;
    /* xmin/xmax are adjacent words: one aligned 8-byte store retires a row. */
    uint64_t *span = (uint64_t *)&fb_dirty_rows[min_row];
    for (uint32_t r = min_row; r <= max_row; r++)
        *span++ = 0;
}

/* Reset the scan bounds and drop whatever the table still describes. */
static void fb_dirty_reset(void) {
    if (fb_dirty_max_row > fb_dirty_min_row &&
        fb_dirty_min_row < FB_DIRTY_MAX_ROWS)
        fb_dirty_clear(fb_dirty_min_row, fb_dirty_max_row - 1);
    fb_dirty_min_row = FB_DIRTY_MAX_ROWS;
    fb_dirty_max_row = 0;
}

void fb_mark_dirty(uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
    if (!w || !h || !fb_global.backbuffer_enabled || !fb_global.backbuffer)
        return;
    if (x >= fb_global.var.xres || y >= fb_global.var.yres)
        return;

    if (x + w > fb_global.var.xres)
        w = fb_global.var.xres - x;
    if (y + h > fb_global.var.yres)
        h = fb_global.var.yres - y;

    /* Whole-screen damage needs no scanline detail: the swap walks the ring
     * in at most two linear segments anyway.  Marking it here keeps clears
     * and scroll redraws O(1) instead of O(rows). */
    if (x == 0 && w == fb_global.var.xres && y == 0 && h == fb_global.var.yres) {
        fb_dirty_full = true;
        fb_global.is_dirty = true;
        return;
    }

    if (y + h > FB_DIRTY_MAX_ROWS) {
        /* Screens taller than the span table fall back to a full copy. */
        fb_dirty_full = true;
        if (y >= FB_DIRTY_MAX_ROWS) {
            fb_global.is_dirty = true;
            return;
        }
        h = FB_DIRTY_MAX_ROWS - y;
    }

    for (uint32_t r = y; r < y + h; r++) {
        struct fb_dirty_row *row = &fb_dirty_rows[r];
        if (x < row->xmin)
            row->xmin = x;
        if (x + w > row->xmax)
            row->xmax = x + w;
    }

    if (y < fb_dirty_min_row)
        fb_dirty_min_row = y;
    if (y + h > fb_dirty_max_row)
        fb_dirty_max_row = y + h;
    fb_global.is_dirty = true;
}

void fb_put_pixel(uint32_t x, uint32_t y, uint32_t color) {
    if (x >= fb_global.var.xres || y >= fb_global.var.yres)
        return;

    void *target = fb_get_target_buffer();
    if (!target)
        return;

    uint32_t row = fb_row_phys(target, y);
    uint32_t *pixel = (uint32_t *)((uint8_t *)target + (uint64_t)row * fb_global.fix.line_length + (uint64_t)x * 4);
    *pixel = color;

    fb_mark_dirty(x, y, 1, 1);
}

void fb_fill_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t color) {
    if (x >= fb_global.var.xres || y >= fb_global.var.yres || !w || !h)
        return;

    if (x + w > fb_global.var.xres)
        w = fb_global.var.xres - x;
    if (y + h > fb_global.var.yres)
        h = fb_global.var.yres - y;

    void *target = fb_get_target_buffer();
    if (!target)
        return;

    uint64_t col64 = ((uint64_t)color << 32) | (uint64_t)color;
    uint32_t pitch = fb_global.fix.line_length;
    uint32_t phys0 = fb_row_phys(target, y);

    if (x == 0 && w == fb_global.var.xres && phys0 + h <= fb_global.var.yres) {
        /* Full-width, physically contiguous rows: one linear sweep. */
        uint32_t *dst = (uint32_t *)((uint8_t *)target + (uint64_t)phys0 * pitch);
        size_t total_pixels = ((size_t)pitch * h) >> 2;
        size_t qwords = total_pixels >> 1;
        uint64_t *d64 = (uint64_t *)dst;
        for (size_t q = 0; q < qwords; q++) {
            d64[q] = col64;
        }
        if (total_pixels & 1) {
            dst[total_pixels - 1] = color;
        }
    } else {
        for (uint32_t r = 0; r < h; r++) {
            uint32_t prow = fb_row_phys(target, y + r);
            uint32_t *dst = (uint32_t *)((uint8_t *)target + (uint64_t)prow * pitch + (uint64_t)x * 4);
            size_t count = w;

            while (count > 0 && ((uintptr_t)dst & 7) != 0) {
                *dst++ = color;
                count--;
            }

            uint64_t *d64 = (uint64_t *)dst;
            size_t qwords = count >> 1;
            for (size_t q = 0; q < qwords; q++) {
                d64[q] = col64;
            }

            if (count & 1) {
                ((uint32_t *)d64)[qwords * 2] = color;
            }
        }
    }

    fb_mark_dirty(x, y, w, h);
}

void fb_clear(uint32_t color) {
    fb_fill_rect(0, 0, fb_global.var.xres, fb_global.var.yres, color);
}

void fb_draw_glyph_scanline(uint32_t x, uint32_t y, uint8_t bits, uint32_t fg, uint32_t bg) {
    if (y >= fb_global.var.yres || x + 8 > fb_global.var.xres)
        return;

    void *target = fb_get_target_buffer();
    if (!target)
        return;

    uint8_t *row = (uint8_t *)target + (uint64_t)fb_row_phys(target, y) * fb_global.fix.line_length + (uint64_t)x * 4;

    uint64_t p0 = (bits & 0x80) ? fg : bg;
    uint64_t p1 = (bits & 0x40) ? fg : bg;
    uint64_t p2 = (bits & 0x20) ? fg : bg;
    uint64_t p3 = (bits & 0x10) ? fg : bg;
    uint64_t p4 = (bits & 0x08) ? fg : bg;
    uint64_t p5 = (bits & 0x04) ? fg : bg;
    uint64_t p6 = (bits & 0x02) ? fg : bg;
    uint64_t p7 = (bits & 0x01) ? fg : bg;

    uint64_t *d64 = (uint64_t *)row;
    d64[0] = (p1 << 32) | p0;
    d64[1] = (p3 << 32) | p2;
    d64[2] = (p5 << 32) | p4;
    d64[3] = (p7 << 32) | p6;

    fb_mark_dirty(x, y, 8, 1);
}

static inline void fb_row_copy(uint8_t *dst, const uint8_t *src, size_t n) {
    if (dst == src || n == 0)
        return;
    if (dst < src) {
        size_t qwords = n >> 3;
        uint64_t *d64 = (uint64_t *)dst;
        const uint64_t *s64 = (const uint64_t *)src;
        for (size_t q = 0; q < qwords; q++) {
            d64[q] = s64[q];
        }
        size_t rem = n & 7;
        if (rem) {
            uint8_t *drem = (uint8_t *)d64 + (qwords << 3);
            const uint8_t *srem = (const uint8_t *)s64 + (qwords << 3);
            for (size_t r = 0; r < rem; r++) {
                drem[r] = srem[r];
            }
        }
    } else {
        dst += n;
        src += n;
        while (n--) {
            *--dst = *--src;
        }
    }
}

void fb_copyarea(const struct fb_copyarea *area) {
    if (!area || !area->width || !area->height)
        return;

    uint32_t dx = area->dx;
    uint32_t dy = area->dy;
    uint32_t sx = area->sx;
    uint32_t sy = area->sy;
    uint32_t w = area->width;
    uint32_t h = area->height;
    uint32_t max_w = fb_global.var.xres;
    uint32_t max_h = fb_global.var.yres;

    if (dx >= max_w || dy >= max_h || sx >= max_w || sy >= max_h)
        return;

    if (dx + w > max_w) w = max_w - dx;
    if (dy + h > max_h) h = max_h - dy;
    if (sx + w > max_w) w = max_w - sx;
    if (sy + h > max_h) h = max_h - sy;

    void *target = fb_get_target_buffer();
    if (!target)
        return;

    uint32_t pitch = fb_global.fix.line_length;
    size_t row_bytes = (size_t)w * 4;

    /* Copy direction follows physical order: the backbuffer ring can place a
     * display row on either side of the wrap, and overlapping moves must be
     * walked away from the overlap. */
    bool bottom_up = fb_row_phys(target, dy) > fb_row_phys(target, sy);

    if (bottom_up) {
        for (int r = (int)h - 1; r >= 0; r--) {
            uint8_t *dst = (uint8_t *)target + (uint64_t)fb_row_phys(target, dy + (uint32_t)r) * pitch + (uint64_t)dx * 4;
            uint8_t *src = (uint8_t *)target + (uint64_t)fb_row_phys(target, sy + (uint32_t)r) * pitch + (uint64_t)sx * 4;
            fb_row_copy(dst, src, row_bytes);
        }
    } else {
        for (uint32_t r = 0; r < h; r++) {
            uint8_t *dst = (uint8_t *)target + (uint64_t)fb_row_phys(target, dy + r) * pitch + (uint64_t)dx * 4;
            uint8_t *src = (uint8_t *)target + (uint64_t)fb_row_phys(target, sy + r) * pitch + (uint64_t)sx * 4;
            fb_row_copy(dst, src, row_bytes);
        }
    }

    fb_mark_dirty(dx, dy, w, h);
}

void fb_imageblit(const struct fb_image *image) {
    if (!image || !image->width || !image->height || !image->data)
        return;

    uint32_t dx = image->dx;
    uint32_t dy = image->dy;
    uint32_t w = image->width;
    uint32_t h = image->height;

    if (dx >= fb_global.var.xres || dy >= fb_global.var.yres)
        return;
    if (dx + w > fb_global.var.xres)
        w = fb_global.var.xres - dx;
    if (dy + h > fb_global.var.yres)
        h = fb_global.var.yres - dy;

    void *target = fb_get_target_buffer();
    if (!target)
        return;

    uint32_t pitch = fb_global.fix.line_length;

    if (image->depth == 1) {
        const uint8_t *src = (const uint8_t *)image->data;
        size_t src_pitch = (image->width + 7) / 8;
        uint64_t fg64 = ((uint64_t)image->fg_color << 32) | image->fg_color;
        uint64_t bg64 = ((uint64_t)image->bg_color << 32) | image->bg_color;

        for (uint32_t y = 0; y < h; y++) {
            const uint8_t *srow = src + (uint64_t)y * src_pitch;
            uint8_t *drow = (uint8_t *)target +
                            (uint64_t)fb_row_phys(target, dy + y) * pitch +
                            (uint64_t)dx * 4;
            uint32_t x = 0;

            /* Unpack one bitmap byte into eight pixels at a time - the same
             * branchless expansion the console glyph renderer uses.  The old
             * per-pixel fb_put_pixel() loop re-checked the bounds and marked
             * a 1x1 dirty rect for every single pixel. */
            for (; x + 8 <= w; x += 8) {
                uint8_t bits = srow[x >> 3];
                uint64_t p0 = (bits & 0x80) ? fg64 : bg64;
                uint64_t p1 = (bits & 0x40) ? fg64 : bg64;
                uint64_t p2 = (bits & 0x20) ? fg64 : bg64;
                uint64_t p3 = (bits & 0x10) ? fg64 : bg64;
                uint64_t p4 = (bits & 0x08) ? fg64 : bg64;
                uint64_t p5 = (bits & 0x04) ? fg64 : bg64;
                uint64_t p6 = (bits & 0x02) ? fg64 : bg64;
                uint64_t p7 = (bits & 0x01) ? fg64 : bg64;
                uint64_t *d64 = (uint64_t *)(drow + (uint64_t)x * 4);

                d64[0] = (p1 << 32) | p0;
                d64[1] = (p3 << 32) | p2;
                d64[2] = (p5 << 32) | p4;
                d64[3] = (p7 << 32) | p6;
            }
            for (; x < w; x++) {
                uint8_t bits = srow[x >> 3];
                bool bit = (bits & (0x80 >> (x & 7))) != 0;
                *(uint32_t *)(drow + (uint64_t)x * 4) =
                    bit ? image->fg_color : image->bg_color;
            }
        }
    } else if (image->depth == 32) {
        const uint32_t *src = (const uint32_t *)image->data;
        for (uint32_t y = 0; y < h; y++) {
            uint8_t *drow = (uint8_t *)target +
                            (uint64_t)fb_row_phys(target, dy + y) * pitch +
                            (uint64_t)dx * 4;
            memcpy(drow, src + (uint64_t)y * image->width, (size_t)w * 4);
        }
    }

    fb_mark_dirty(dx, dy, w, h);
}

void fb_swap_buffer_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
    if (!fb_global.backbuffer || !fb_global.screen_base)
        return;

    uint32_t max_w = fb_global.var.xres;
    uint32_t max_h = fb_global.var.yres;

    if (x >= max_w || y >= max_h || !w || !h)
        return;

    if (x + w > max_w) w = max_w - x;
    if (y + h > max_h) h = max_h - y;

    uint32_t pitch = fb_global.fix.line_length;
    uint8_t *front = (uint8_t *)fb_global.screen_base;
    uint8_t *back = (uint8_t *)fb_global.backbuffer;

    /* The frontbuffer is write-combining (fb_map_wc), so stream the copy with
     * non-temporal stores instead of one cached store per qword: a full-screen
     * swap is 4-8 MB and cacheable stores to WC memory are the most expensive
     * way to move it.  memcpy_to_wc() picks movnti on hardware and rep movsq
     * under QEMU TCG, where emulating movnti is the slower option. */
    if (x == 0 && w == max_w) {
        /* Full-width: the source rows form one contiguous physical range up
         * to the ring wrap, so walk it in at most two linear copies instead
         * of one call per scanline.  The origin is non-zero after any console
         * scroll, so this is the common full-screen case. */
        uint32_t done = 0;
        while (done < h) {
            uint32_t phys = fb_row_phys(back, y + done);
            uint32_t chunk = h - done;
            if (phys + chunk > max_h)
                chunk = max_h - phys;
            memcpy_to_wc(front + (uint64_t)(y + done) * pitch,
                         back + (uint64_t)phys * pitch,
                         (size_t)pitch * chunk);
            done += chunk;
        }
    } else {
        size_t row_bytes = (size_t)w * 4;
        for (uint32_t r = 0; r < h; r++) {
            uint32_t prow = fb_row_phys(back, y + r);
            memcpy_to_wc(front + (uint64_t)(y + r) * pitch + (uint64_t)x * 4,
                         back + (uint64_t)prow * pitch + (uint64_t)x * 4,
                         row_bytes);
        }
    }

    /* Non-temporal stores retire out of order; fence them out before the
     * display engine (or another CPU) is allowed to look at the aperture. */
    __asm__ volatile("sfence" ::: "memory");
}

void fb_swap_buffer(void) {
    if (!fb_global.is_dirty || !fb_global.backbuffer || !fb_global.screen_base)
        return;

    uint32_t max_w = fb_global.var.xres;
    uint32_t max_h = fb_global.var.yres;
    fb_global.is_dirty = false;

    if (fb_dirty_full) {
        fb_dirty_full = false;
        fb_dirty_reset();
        fb_swap_buffer_rect(0, 0, max_w, max_h);
        return;
    }

    if (fb_dirty_min_row > fb_dirty_max_row || fb_dirty_min_row >= max_h) {
        fb_dirty_min_row = FB_DIRTY_MAX_ROWS;
        fb_dirty_max_row = 0;
        return;
    }

    uint32_t min_row = fb_dirty_min_row;
    uint32_t max_row = fb_dirty_max_row - 1;
    if (max_row >= FB_DIRTY_MAX_ROWS)
        max_row = FB_DIRTY_MAX_ROWS - 1;

    uint32_t pitch = fb_global.fix.line_length;
    uint8_t *front = (uint8_t *)fb_global.screen_base;
    uint8_t *back = (uint8_t *)fb_global.backbuffer;

    uint32_t r = min_row;
    while (r <= max_row) {
        struct fb_dirty_row *row = &fb_dirty_rows[r];
        uint32_t x, xmax, run_end, done;
        size_t row_bytes;
        bool full_row;

        if (row->xmin >= row->xmax) {
            r++;
            continue;
        }

        x = row->xmin;
        xmax = row->xmax;
        if (xmax > max_w)
            xmax = max_w;
        if (x >= xmax) {
            r++;
            continue;
        }
        row_bytes = (size_t)(xmax - x) * 4;
        full_row = (x == 0 && xmax >= max_w);

        /* Coalesce one run of scanlines that carry this exact span: a glyph
         * is 16 identical rows and a redrawn text line is 16 more, so this
         * turns what used to be one copy call per scanline into one call per
         * run (or per ring segment, below). */
        run_end = r;
        while (run_end < max_row &&
               fb_dirty_rows[run_end + 1].xmin == x &&
               fb_dirty_rows[run_end + 1].xmax == xmax)
            run_end++;

        /* The backbuffer is a ring.  Split the run where the physical row
         * number wraps so every copy reads a linear range, and stitch
         * full-width rows into a single bulk copy per segment. */
        done = 0;
        while (r + done <= run_end) {
            uint32_t first = r + done;
            uint32_t phys = fb_row_phys(back, first);
            uint32_t chunk = run_end - first + 1;
            if (phys + chunk > max_h)
                chunk = max_h - phys;

            if (full_row) {
                memcpy_to_wc(front + (uint64_t)first * pitch,
                             back + (uint64_t)phys * pitch,
                             (size_t)pitch * chunk);
            } else {
                for (uint32_t i = 0; i < chunk; i++) {
                    memcpy_to_wc(front + (uint64_t)(first + i) * pitch + (uint64_t)x * 4,
                                 back + (uint64_t)(phys + i) * pitch + (uint64_t)x * 4,
                                 row_bytes);
                }
            }
            done += chunk;
        }

        r = run_end + 1;
    }

    fb_dirty_reset();
    __asm__ volatile("sfence" ::: "memory");
}
