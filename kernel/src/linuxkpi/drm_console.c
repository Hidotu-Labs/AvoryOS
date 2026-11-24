/* Native framebuffer snapshot accessor and WC copy bridge for LinuxKPI. */

#include "linuxkpi/drm_console.h"

#include "fb/framebuffer.h"
#include "lib/string.h"

const void *linuxkpi_native_fb_snapshot(unsigned int *w, unsigned int *h,
                                        unsigned int *pitch,
                                        unsigned int *y_origin,
                                        _Bool *is_graphics) {
  void *back = fb_get_backbuffer();

  if (w)
    *w = fb_get_width();
  if (h)
    *h = fb_get_height();
  if (pitch)
    *pitch = fb_get_pitch();
  if (is_graphics)
    *is_graphics = (fb_get_kd_mode() == KD_GRAPHICS);

  if (back) {
    if (y_origin)
      *y_origin = fb_backbuffer_row(0);
    return back;
  }

  if (y_origin)
    *y_origin = 0;
  return fb_get_base();
}

void linuxkpi_memcpy_to_wc(void *dst, const void *src, unsigned long n) {
  memcpy_to_wc(dst, src, n);
}
