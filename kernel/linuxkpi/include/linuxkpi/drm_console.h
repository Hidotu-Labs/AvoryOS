#ifndef LINUXKPI_DRM_CONSOLE_H
#define LINUXKPI_DRM_CONSOLE_H

/* Native <-> LinuxKPI console framebuffer bridge for drm_client_setup().
 *
 * Uses only standard C types so both kernel/src/fb/ and kernel/linuxkpi/src/
 * can include it cleanly. */

_Bool linuxkpi_drm_console_owns_scanout(void);

void linuxkpi_drm_console_blit_rect(const void *src_base, unsigned int src_w,
                                    unsigned int src_h, unsigned int src_pitch,
                                    unsigned int src_y_off,
                                    unsigned int dst_x, unsigned int dst_y,
                                    unsigned int w, unsigned int h);

const void *linuxkpi_native_fb_snapshot(unsigned int *w, unsigned int *h,
                                        unsigned int *pitch,
                                        unsigned int *y_origin,
                                        _Bool *is_graphics);

void linuxkpi_memcpy_to_wc(void *dst, const void *src, unsigned long n);

#endif /* LINUXKPI_DRM_CONSOLE_H */
