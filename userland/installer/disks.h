#ifndef AVORY_INSTALLER_DISKS_H
#define AVORY_INSTALLER_DISKS_H

#include "installer.h"

int disks_scan(disk_t *out, int max_disks);
void disks_format_size(uint64_t bytes, char *buf, size_t buf_sz);

int disks_partition_and_format(const disk_t *d,
                               char *out_esp, size_t esp_sz,
                               char *out_root, size_t root_sz,
                               install_progress_fn cb, void *ctx);

bool disks_get_uuid(const char *dev_path, char *out_uuid, size_t uuid_sz);

#endif /* AVORY_INSTALLER_DISKS_H */
