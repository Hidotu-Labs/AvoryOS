#ifndef FB_FRAMEBUFFER_H
#define FB_FRAMEBUFFER_H

#include <limine.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "fb.h"
#include "terminal.h"

struct vfs_node;

/* ── Lifecycle & Hardware Setup ──────────────────────────────────────────── */
void fb_init(struct limine_framebuffer *framebuffer);
void fb_map_wc(void);
void fb_register_vfs(void);
void fb_detect_drm_backend(void);

/* ── Buffer & Mode State ─────────────────────────────────────────────────── */
void *fb_get_base(void);
void *fb_get_backbuffer(void);
bool fb_is_backbuffer_enabled(void);
void fb_set_backbuffer_mode(bool enabled);

/* ── Screen Geometry & Info ──────────────────────────────────────────────── */
uint32_t fb_get_width(void);
uint32_t fb_get_height(void);
uint32_t fb_get_pitch(void);
uint32_t fb_get_bpp(void);
uint64_t fb_get_size(void);
struct fb_info *fb_get_info(void);

/* ── Optimized Drawing Primitives ────────────────────────────────────────── */
void fb_clear(uint32_t color);
void fb_put_pixel(uint32_t x, uint32_t y, uint32_t color);
void fb_fill_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t color);
void fb_draw_glyph_scanline(uint32_t x, uint32_t y, uint8_t bits, uint32_t fg, uint32_t bg);
void fb_copyarea(const struct fb_copyarea *area);
void fb_imageblit(const struct fb_image *image);

/* ── Dirty Tracking & Double Buffer Swap ─────────────────────────────────── */
void fb_mark_dirty(uint32_t x, uint32_t y, uint32_t w, uint32_t h);
void fb_swap_buffer(void);
void fb_swap_buffer_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h);

/* ── Console / VT Mode Controls ──────────────────────────────────────────── */
int fb_get_kd_mode(void);
void fb_set_kd_mode(int mode);

/* ── Device Registry & VFS Integration ───────────────────────────────────── */
struct vfs_node *fb_lookup_device(const char *name);
void fb_register_device_node(const char *name, struct vfs_node *node);

#endif /* FB_FRAMEBUFFER_H */
