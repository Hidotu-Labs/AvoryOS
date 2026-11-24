#ifndef AVORY_INSTALLER_H
#define AVORY_INSTALLER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define INSTALLER_VERSION "2.0.0"
#define TARGET_MOUNT      "/mnt/target"
#define LOG_PATH          "/tmp/avory-installer.log"
#define MAX_DISKS         32
#define MAX_PARTITIONS    16

typedef struct {
    char name[32];
    char dev_path[64];
    char model[64];
    uint64_t size_bytes;
    bool is_live_media;
    bool is_nvme;
    bool is_removable;
} disk_t;

typedef enum {
    PART_NONE = 0, /* ignored existing partition */
    PART_ESP,
    PART_ROOT,
    PART_HOME,
    PART_VAR,
    PART_SWAP,
    PART_CUSTOM,
} part_role_t;

typedef enum {
    FS_FAT32 = 0,
    FS_EXT4,
    FS_SWAP,
    FS_OTHER,
} part_fs_t;

typedef enum {
    SCHEME_SINGLE_ROOT = 0,
    SCHEME_ROOT_SWAP,
    SCHEME_ROOT_HOME,
    SCHEME_ROOT_HOME_SWAP,
    SCHEME_CUSTOM,
    SCHEME_EXISTING,
} part_scheme_t;

typedef struct {
    part_role_t role;
    part_fs_t fs;
    char mountpoint[64];
    char label[32];
    uint64_t start_mib;
    uint64_t size_mib; /* 0 = fill remaining space (whole-disk mode) */
    int part_num;      /* 1-based partition index on disk */
    bool existing;     /* already present on disk */
    bool is_free;      /* unallocated free-space gap */
    bool format;       /* format before installing */
    char cur_fs[24];   /* detected existing filesystem */
    char dev_path[80]; /* populated after scan/partitioning */
} part_entry_t;

typedef enum {
    DE_PLASMA = 0,
    DE_XFCE4  = 1,
    DE_ICEWM  = 2,
    DE_CONSOLE = 3,
} desktop_choice_t;

typedef struct {
    char keymap[32];
    char hostname[64];
    bool enable_dhcp;
    int disk_idx;
    bool disk_confirmed;
    part_scheme_t part_scheme;
    bool wipe_disk;
    part_entry_t parts[MAX_PARTITIONS];
    int part_count;
    char fullname[64];
    char username[32];
    char user_password[64];
    char root_password[64];
    desktop_choice_t desktop;
    bool dry_run;
} installer_cfg_t;

typedef void (*install_progress_fn)(int pct, const char *stage, const char *log_line, void *ctx);

#endif /* AVORY_INSTALLER_H */
