#ifndef __AVORY_LINUXKPI_DRM_DP_HELPER_H
#define __AVORY_LINUXKPI_DRM_DP_HELPER_H

/* AvoryOS overlay for <drm/display/drm_dp_helper.h>.
 *
 * The stock header embeds `struct delayed_work` (drm_dp_aux::crc_work,
 * ::unregister_work) but, like some other DRM headers, relies on the includer
 * having pulled <linux/workqueue.h> first.  With the overlay header set that
 * assumption is fragile, so this wrapper includes it explicitly and then the
 * stock header via #include_next.  No other change. */

#include <linux/workqueue.h>

#include_next <drm/display/drm_dp_helper.h>

#endif /* __AVORY_LINUXKPI_DRM_DP_HELPER_H */
