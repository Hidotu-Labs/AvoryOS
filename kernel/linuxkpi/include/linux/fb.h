#ifndef __AVORY_LINUXKPI_FB_H
#define __AVORY_LINUXKPI_FB_H

/* AvoryOS overlay for <linux/fb.h>.
 *
 * Imported DRM core code only needs the mode-clock macros from this header
 * (drm_modes.c and drm_edid.c use KHZ2PICOS).  Upstream's fb.h includes the
 * full i2c, backlight, regulator, suspend and swap chains just to declare
 * fb_info; fbdev emulation is off and nothing in the compiled set touches an
 * fb_info.  The macro values are copied from upstream uapi/linux/fb.h; do not
 * include that header here (it pulls <linux/i2c.h> and the same chain). */

#include <linux/types.h>

struct fb_bitfield {
  __u32 offset;
  __u32 length;
  __u32 msb_right;
};

#define PICOS2KHZ(a) (1000000000UL / (a))
#define KHZ2PICOS(a) (1000000000UL / (a))

/* Framebuffer blanking levels and the maximum fb count (upstream uapi
 * values).  <linux/backlight.h>'s inline helpers assign these to fb_blank;
 * fbdev emulation is off, but the constants are part of this header's
 * contract for the imported backlight code. */
#define FB_MAX 32
#define FB_BLANK_UNBLANK 0
#define FB_BLANK_NORMAL 1
#define FB_BLANK_VSYNC_SUSPEND 2
#define FB_BLANK_HSYNC_SUSPEND 3
#define FB_BLANK_POWERDOWN 4

#endif /* __AVORY_LINUXKPI_FB_H */
