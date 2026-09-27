#include "framebuffer.h"
#include "../lib/string.h"

extern struct fb_info fb_global;
extern void *fb_get_target_buffer(void);

void fb_mark_dirty(uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
    if (!w || !h || x >= fb_global.var.xres || y >= fb_global.var.yres)
        return;

    if (x + w > fb_global.var.xres)
        w = fb_global.var.xres - x;
    if (y + h > fb_global.var.yres)
        h = fb_global.var.yres - y;

    if (!fb_global.is_dirty) {
        fb_global.dirty_min_x = x;
        fb_global.dirty_min_y = y;
        fb_global.dirty_max_x = x + w;
        fb_global.dirty_max_y = y + h;
        fb_global.is_dirty = true;
    } else {
        if (x < fb_global.dirty_min_x)
            fb_global.dirty_min_x = x;
        if (y < fb_global.dirty_min_y)
            fb_global.dirty_min_y = y;
        if (x + w > fb_global.dirty_max_x)
            fb_global.dirty_max_x = x + w;
        if (y + h > fb_global.dirty_max_y)
            fb_global.dirty_max_y = y + h;
    }
}

void fb_put_pixel(uint32_t x, uint32_t y, uint32_t color) {
    if (x >= fb_global.var.xres || y >= fb_global.var.yres)
        return;

    void *target = fb_get_target_buffer();
    if (!target)
        return;

    uint32_t *pixel = (uint32_t *)((uint8_t *)target + (uint64_t)y * fb_global.fix.line_length + (uint64_t)x * 4);
    *pixel = color;

    if (fb_global.backbuffer_enabled && fb_global.backbuffer) {
        fb_mark_dirty(x, y, 1, 1);
    }
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

    if (x == 0 && w == fb_global.var.xres) {
        uint32_t *dst = (uint32_t *)((uint8_t *)target + (uint64_t)y * pitch);
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
            uint32_t *dst = (uint32_t *)((uint8_t *)target + (uint64_t)(y + r) * pitch + (uint64_t)x * 4);
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

    if (fb_global.backbuffer_enabled && fb_global.backbuffer) {
        fb_mark_dirty(x, y, w, h);
    }
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

    uint8_t *row = (uint8_t *)target + (uint64_t)y * fb_global.fix.line_length + (uint64_t)x * 4;

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

    if (fb_global.backbuffer_enabled && fb_global.backbuffer) {
        fb_mark_dirty(x, y, 8, 1);
    }
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

    if (dy > sy) {
        for (int r = (int)h - 1; r >= 0; r--) {
            uint8_t *dst = (uint8_t *)target + (uint64_t)(dy + r) * pitch + (uint64_t)dx * 4;
            uint8_t *src = (uint8_t *)target + (uint64_t)(sy + r) * pitch + (uint64_t)sx * 4;
            fb_row_copy(dst, src, row_bytes);
        }
    } else {
        for (uint32_t r = 0; r < h; r++) {
            uint8_t *dst = (uint8_t *)target + (uint64_t)(dy + r) * pitch + (uint64_t)dx * 4;
            uint8_t *src = (uint8_t *)target + (uint64_t)(sy + r) * pitch + (uint64_t)sx * 4;
            fb_row_copy(dst, src, row_bytes);
        }
    }

    if (fb_global.backbuffer_enabled && fb_global.backbuffer) {
        fb_mark_dirty(dx, dy, w, h);
    }
}

void fb_imageblit(const struct fb_image *image) {
    if (!image || !image->width || !image->height || !image->data)
        return;

    uint32_t dx = image->dx;
    uint32_t dy = image->dy;
    uint32_t w = image->width;
    uint32_t h = image->height;

    if (image->depth == 1) {
        const uint8_t *src = (const uint8_t *)image->data;
        size_t src_pitch = (w + 7) / 8;
        for (uint32_t y = 0; y < h; y++) {
            for (uint32_t x = 0; x < w; x++) {
                uint8_t byte = src[y * src_pitch + (x / 8)];
                bool bit = (byte & (0x80 >> (x % 8))) != 0;
                fb_put_pixel(dx + x, dy + y, bit ? image->fg_color : image->bg_color);
            }
        }
    } else if (image->depth == 32) {
        const uint32_t *src = (const uint32_t *)image->data;
        for (uint32_t y = 0; y < h; y++) {
            for (uint32_t x = 0; x < w; x++) {
                fb_put_pixel(dx + x, dy + y, src[y * w + x]);
            }
        }
    }
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

    /* The frontbuffer is write-combining (fb_map_wc), so stream the copy with
     * non-temporal stores instead of one cached store per qword: a full-screen
     * swap is 4-8 MB and cacheable stores to WC memory are the most expensive
     * way to move it.  memcpy_to_wc() picks movnti on hardware and rep movsq
     * under QEMU TCG, where emulating movnti is the slower option. */
    if (x == 0 && w == max_w) {
        memcpy_to_wc((uint8_t *)fb_global.screen_base + (uint64_t)y * pitch,
                     (const uint8_t *)fb_global.backbuffer + (uint64_t)y * pitch,
                     (size_t)pitch * h);
    } else {
        size_t row_bytes = (size_t)w * 4;
        for (uint32_t r = 0; r < h; r++) {
            memcpy_to_wc((uint8_t *)fb_global.screen_base + (uint64_t)(y + r) * pitch + (uint64_t)x * 4,
                         (const uint8_t *)fb_global.backbuffer + (uint64_t)(y + r) * pitch + (uint64_t)x * 4,
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

    uint32_t min_x = fb_global.dirty_min_x;
    uint32_t min_y = fb_global.dirty_min_y;
    uint32_t max_x = fb_global.dirty_max_x;
    uint32_t max_y = fb_global.dirty_max_y;

    fb_global.is_dirty = false;

    if (min_x >= max_x || min_y >= max_y)
        return;

    uint32_t w = max_x - min_x;
    uint32_t h = max_y - min_y;

    fb_swap_buffer_rect(min_x, min_y, w, h);
}
