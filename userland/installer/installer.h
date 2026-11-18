#ifndef AVORY_INSTALLER_H
#define AVORY_INSTALLER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define INSTALLER_VERSION "2.0.0"
#define TARGET_MOUNT      "/mnt/target"
#define LOG_PATH          "/tmp/avory-installer.log"
#define MAX_DISKS         32

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
    char fullname[64];
    char username[32];
    char user_password[64];
    char root_password[64];
    desktop_choice_t desktop;
    bool dry_run;
} installer_cfg_t;

typedef void (*install_progress_fn)(int pct, const char *stage, const char *log_line, void *ctx);

#endif /* AVORY_INSTALLER_H */
