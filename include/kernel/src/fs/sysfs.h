#ifndef FS_SYSFS_H
#define FS_SYSFS_H
#include <stdbool.h>
#include <stdint.h>

void sysfs_init(void);
void sysfs_populate_network(void);
void sysfs_gpu_update_connector(uint32_t scanout, bool connected, const char *modes);

// GPU device path for netlink uevents (e.g. "/devices/pci0000:00/0000:00:02.0/drm/card0")
extern char sysfs_gpu_devpath[128];
extern char sysfs_gpu_connector_devpath[128];

#endif
