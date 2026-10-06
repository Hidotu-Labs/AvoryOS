#ifndef FB_VBLANK_H
#define FB_VBLANK_H

#include <stdint.h>

/* Software vblank engine: the presentation hardware (virtio-gpu/GOP) exposes
 * no scanout interrupt, so frame boundaries are synthesized from the
 * monotonic clock at the display refresh rate (60 Hz unless the EDID/KMS
 * mode says otherwise). Every flip-complete / WAIT_VBLANK timestamp and
 * sequence number comes from here, which is what paces compositors (KWin,
 * Weston) to the refresh rate instead of letting them present uncapped. */
void vblank_set_refresh_hz(uint32_t hz);
uint64_t vblank_period_ns(void);
/* Boundaries elapsed since engine epoch (wraps naturally at 2^32). */
uint32_t vblank_sequence(void);
/* Wall time of a boundary, for event timestamps. */
void vblank_timestamp(uint32_t seq, uint32_t *sec, uint32_t *usec);
/* Sleep until the next boundary. 0 on success, -4 (-EINTR) on signal. */
int64_t vblank_wait_next(void);
/* Sleep until `target` has passed (wraparound-safe). 0 or -4. */
int64_t vblank_wait_for(uint32_t target);

#endif
