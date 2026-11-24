#ifndef AVORY_INSTALLER_DISKS_H
#define AVORY_INSTALLER_DISKS_H

#include "installer.h"

int disks_scan(disk_t *out, int max_disks);
void disks_format_size(uint64_t bytes, char *buf, size_t buf_sz);

const char *disks_scheme_name(part_scheme_t scheme);
const char *disks_fs_name(part_fs_t fs);
void disks_init_preset(part_scheme_t scheme, uint64_t disk_bytes,
                       part_entry_t *out, int *out_count);
bool disks_validate_parts(const part_entry_t *parts, int count,
                          uint64_t disk_bytes, uint64_t min_root_bytes,
                          bool wipe_disk, char *err, size_t err_sz);
void disks_format_part_summary(const part_entry_t *parts, int count,
                               char *buf, size_t buf_sz);

int disks_scan_partitions(const disk_t *d, part_entry_t *out, int max_parts);
bool disks_check_ext4_compat(const char *dev_path, char *err, size_t err_sz);
int disks_delete_partition(const disk_t *d, int part_num);
int disks_shrink_partition(const disk_t *d, const part_entry_t *p,
                           uint64_t new_size_mib, char *err, size_t err_sz);

int disks_partition_and_format(const disk_t *d,
                               part_entry_t *parts, int count,
                               install_progress_fn cb, void *ctx);

int disks_apply_existing(const disk_t *d,
                         part_entry_t *parts, int count,
                         install_progress_fn cb, void *ctx);

bool disks_get_uuid(const char *dev_path, char *out_uuid, size_t uuid_sz);

#endif /* AVORY_INSTALLER_DISKS_H */
