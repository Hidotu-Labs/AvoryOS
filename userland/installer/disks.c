#include "disks.h"
#include "install.h"

#include <ctype.h>
#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <unistd.h>

static bool is_whole_disk_name(const char *name) {
    if (!name || !name[0]) return false;
    if (strncmp(name, "loop", 4) == 0) return false;
    if (strncmp(name, "ram", 3) == 0) return false;
    if (strncmp(name, "zram", 4) == 0) return false;
    if (strncmp(name, "sr", 2) == 0) return false;
    if (strncmp(name, "fd", 2) == 0) return false;
    if (strncmp(name, "dm-", 3) == 0) return false;
    if (strncmp(name, "nbd", 3) == 0) return false;

    /* nvme0n1 is whole disk; nvme0n1p1 is partition */
    if (strncmp(name, "nvme", 4) == 0) {
        const char *p = strchr(name, 'p');
        if (p && isdigit((unsigned char)p[1])) return false;
        return true;
    }

    /* AvoryOS AHCI/ATA whole disks: sata0..sata9, ata0..ata9
     * (partitions are sata01, sata02, ... or sata0p1, ...). */
    if (strncmp(name, "sata", 4) == 0) {
        return isdigit((unsigned char)name[4]) && name[5] == '\0';
    }
    if (strncmp(name, "ata", 3) == 0) {
        return isdigit((unsigned char)name[3]) && name[4] == '\0';
    }

    /* sdX, vdX, hdX: trailing digits indicate partition */
    size_t len = strlen(name);
    if (len > 0 && isdigit((unsigned char)name[len - 1])) {
        return false;
    }
    return true;
}

static bool read_sysfs_line(const char *path, char *buf, size_t sz) {
    FILE *f = fopen(path, "r");
    if (!f) return false;
    bool ok = fgets(buf, (int)sz, f) != NULL;
    fclose(f);
    if (!ok) return false;
    size_t len = strlen(buf);
    while (len > 0 && (buf[len - 1] == '\n' || buf[len - 1] == '\r' || buf[len - 1] == ' ')) {
        buf[--len] = '\0';
    }
    return true;
}

static bool dev_matches_disk(const char *dev_path, const char *disk_name) {
    if (strncmp(dev_path, "/dev/", 5) != 0) return false;
    const char *base = dev_path + 5;
    size_t nlen = strlen(disk_name);
    if (strncmp(base, disk_name, nlen) != 0) return false;
    char next = base[nlen];
    if (next == '\0') return true;
    if (isdigit((unsigned char)next)) return true;
    if (next == 'p' && isdigit((unsigned char)base[nlen + 1])) return true;
    return false;
}

static bool is_sysfs_partition(const char *name) {
    char path[256];
    char buf[256];
    snprintf(path, sizeof(path), "/sys/block/%s/uevent", name);
    FILE *f = fopen(path, "r");
    if (!f) return false;
    bool is_part = false;
    while (fgets(buf, sizeof(buf), f)) {
        if (strncmp(buf, "DEVTYPE=partition", 17) == 0) {
            is_part = true;
            break;
        }
    }
    fclose(f);
    return is_part;
}

int disks_scan(disk_t *out, int max_disks) {
    int count = 0;
    DIR *dir = opendir("/sys/block");
    if (!dir) return 0;

    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL && count < max_disks) {
        if (ent->d_name[0] == '.') continue;
        if (!is_whole_disk_name(ent->d_name)) continue;
        if (is_sysfs_partition(ent->d_name)) continue;

        char path[256];
        char val[128];
        uint64_t sectors = 0;
        snprintf(path, sizeof(path), "/sys/block/%s/size", ent->d_name);
        if (read_sysfs_line(path, val, sizeof(val))) {
            sectors = strtoull(val, NULL, 10);
        }
        if (sectors == 0) continue;

        disk_t *d = &out[count++];
        memset(d, 0, sizeof(*d));
        snprintf(d->name, sizeof(d->name), "%s", ent->d_name);
        snprintf(d->dev_path, sizeof(d->dev_path), "/dev/%s", ent->d_name);
        d->size_bytes = sectors * 512ULL;
        d->is_nvme = (strncmp(ent->d_name, "nvme", 4) == 0);

        snprintf(path, sizeof(path), "/sys/block/%s/removable", ent->d_name);
        if (read_sysfs_line(path, val, sizeof(val))) {
            d->is_removable = (atoi(val) != 0);
        }

        snprintf(path, sizeof(path), "/sys/block/%s/device/model", ent->d_name);
        if (!read_sysfs_line(path, d->model, sizeof(d->model))) {
            snprintf(d->model, sizeof(d->model), "%s",
                     d->is_nvme ? "NVMe Disk"
                                : (strncmp(ent->d_name, "sata", 4) == 0 ? "SATA Disk" : "Block Disk"));
        }
    }
    closedir(dir);

    FILE *mounts = fopen("/proc/mounts", "r");
    if (mounts) {
        char line[512];
        while (fgets(line, sizeof(line), mounts)) {
            char mdev[128] = {0}, mpoint[128] = {0};
            if (sscanf(line, "%127s %127s", mdev, mpoint) == 2) {
                if (strcmp(mpoint, "/") == 0) {
                    for (int i = 0; i < count; i++) {
                        if (dev_matches_disk(mdev, out[i].name)) {
                            out[i].is_live_media = true;
                        }
                    }
                }
            }
        }
        fclose(mounts);
    }

    return count;
}

void disks_format_size(uint64_t bytes, char *buf, size_t buf_sz) {
    double gib = (double)bytes / (1024.0 * 1024.0 * 1024.0);
    if (gib >= 1024.0) {
        snprintf(buf, buf_sz, "%.1f TiB", gib / 1024.0);
    } else if (gib >= 1.0) {
        snprintf(buf, buf_sz, "%.1f GiB", gib);
    } else {
        double mib = (double)bytes / (1024.0 * 1024.0);
        snprintf(buf, buf_sz, "%.0f MiB", mib);
    }
}

const char *disks_scheme_name(part_scheme_t scheme) {
    switch (scheme) {
        case SCHEME_SINGLE_ROOT:    return "Single Root (ESP + /)";
        case SCHEME_ROOT_SWAP:      return "Root + Swap (ESP + swap + /)";
        case SCHEME_ROOT_HOME:      return "Separate /home (ESP + / + /home)";
        case SCHEME_ROOT_HOME_SWAP: return "Root + /home + Swap";
        case SCHEME_CUSTOM:         return "Custom partition layout";
        case SCHEME_EXISTING:       return "Use existing partitions (non-destructive)";
        default:                    return "Unknown";
    }
}

const char *disks_fs_name(part_fs_t fs) {
    switch (fs) {
        case FS_FAT32: return "fat32";
        case FS_EXT4:  return "ext4";
        case FS_SWAP:  return "swap";
        case FS_OTHER: return "other";
        default:       return "unknown";
    }
}

static void set_part(part_entry_t *p, part_role_t role, part_fs_t fs,
                     const char *mnt, const char *lbl, uint64_t size_mib) {
    memset(p, 0, sizeof(*p));
    p->role = role;
    p->fs = fs;
    p->format = true;
    snprintf(p->mountpoint, sizeof(p->mountpoint), "%s", mnt);
    snprintf(p->label, sizeof(p->label), "%s", lbl);
    p->size_mib = size_mib;
}

void disks_init_preset(part_scheme_t scheme, uint64_t disk_bytes,
                       part_entry_t *out, int *out_count) {
    uint64_t disk_mib = disk_bytes / (1024ULL * 1024ULL);
    uint64_t avail = (disk_mib > 130) ? (disk_mib - 130) : 0;
    uint64_t swap_mib = (avail >= 16384) ? 2048 : ((avail >= 6144) ? 1024 : 512);

    int n = 0;
    set_part(&out[n++], PART_ESP, FS_FAT32, "/boot/efi", "EFI", 128);

    if (scheme == SCHEME_ROOT_SWAP || scheme == SCHEME_ROOT_HOME_SWAP) {
        set_part(&out[n++], PART_SWAP, FS_SWAP, "swap", "AvoryOS-Swap", swap_mib);
    }

    if (scheme == SCHEME_ROOT_HOME || scheme == SCHEME_ROOT_HOME_SWAP) {
        uint64_t rem = (scheme == SCHEME_ROOT_HOME_SWAP && avail > swap_mib) ? (avail - swap_mib) : avail;
        uint64_t root_mib = (rem >= 32768) ? 20480 : (rem / 2);
        if (root_mib < 2048 && rem >= 4096) root_mib = 2048;
        set_part(&out[n++], PART_ROOT, FS_EXT4, "/", "AvoryOS-Root", root_mib);
        set_part(&out[n++], PART_HOME, FS_EXT4, "/home", "AvoryOS-Home", 0);
    } else {
        set_part(&out[n++], PART_ROOT, FS_EXT4, "/", "AvoryOS-Root", 0);
    }

    *out_count = n;
}

static bool has_feature_token(const char *line, const char *tok) {
    size_t tlen = strlen(tok);
    const char *p = line;
    while ((p = strstr(p, tok)) != NULL) {
        bool left_ok = (p == line || p[-1] == ' ' || p[-1] == '\t' || p[-1] == ':');
        char right = p[tlen];
        bool right_ok = (right == '\0' || right == ' ' || right == '\t' || right == '\n' || right == '\r');
        if (left_ok && right_ok) return true;
        p += tlen;
    }
    return false;
}

bool disks_check_ext4_compat(const char *dev_path, char *err, size_t err_sz) {
    const char *t_argv[] = {"tune2fs", "-l", dev_path, NULL};
    char out[4096] = {0};
    if (run_cmd_capture(t_argv, out, sizeof(out)) != 0) {
        const char *d_argv[] = {"dumpe2fs", "-h", dev_path, NULL};
        if (run_cmd_capture(d_argv, out, sizeof(out)) != 0) {
            return true; /* tool unavailable; allow and let mount decide */
        }
    }
    char *feat = strstr(out, "Filesystem features:");
    if (!feat) return true;
    char *eol = strchr(feat, '\n');
    if (eol) *eol = '\0';

    const char *bad[] = {
        "flex_bg", "metadata_csum", "64bit", "huge_file",
        "large_file", "dir_nlink", "extra_isize", "orphan_file", NULL
    };
    for (int i = 0; bad[i]; i++) {
        if (has_feature_token(feat, bad[i])) {
            snprintf(err, err_sz,
                     "%s uses ext4 '%s' (unsupported by AvoryOS kernel); enable Format.",
                     dev_path, bad[i]);
            return false;
        }
    }
    return true;
}

bool disks_validate_parts(const part_entry_t *parts, int count,
                          uint64_t disk_bytes, uint64_t min_root_bytes,
                          bool wipe_disk, char *err, size_t err_sz) {
    if (count < 2 || count > MAX_PARTITIONS) {
        snprintf(err, err_sz, "Need between 2 and %d entries.", MAX_PARTITIONS);
        return false;
    }

    int esp_count = 0, root_count = 0, fill_count = 0;
    uint64_t fixed_mib = 0;
    int root_idx = -1;

    for (int i = 0; i < count; i++) {
        const part_entry_t *p = &parts[i];
        if (p->is_free || p->role == PART_NONE) continue;

        if (p->role == PART_ESP) {
            esp_count++;
            if (p->fs != FS_FAT32) {
                snprintf(err, err_sz, "ESP partition must use fat32.");
                return false;
            }
            if (p->size_mib != 0 && p->size_mib < 128) {
                snprintf(err, err_sz, "ESP partition must be at least 128 MiB.");
                return false;
            }
        } else if (p->role == PART_ROOT || strcmp(p->mountpoint, "/") == 0) {
            root_count++;
            root_idx = i;
            if (p->fs != FS_EXT4) {
                snprintf(err, err_sz, "Root (/) partition must use ext4.");
                return false;
            }
            if (!wipe_disk && !p->format) {
                snprintf(err, err_sz, "Root (/) partition must be formatted.");
                return false;
            }
        } else if (p->fs == FS_EXT4) {
            if (p->mountpoint[0] != '/') {
                snprintf(err, err_sz, "Partition %d mountpoint must start with '/'.", i + 1);
                return false;
            }
            if (!wipe_disk && p->existing && !p->format) {
                if (!disks_check_ext4_compat(p->dev_path, err, err_sz)) {
                    return false;
                }
            }
        }

        if (p->fs != FS_SWAP && p->role != PART_ESP) {
            for (int j = i + 1; j < count; j++) {
                if (parts[j].is_free || parts[j].role == PART_NONE) continue;
                if (parts[j].fs != FS_SWAP && strcmp(p->mountpoint, parts[j].mountpoint) == 0) {
                    snprintf(err, err_sz, "Duplicate mountpoint: %s", p->mountpoint);
                    return false;
                }
            }
        }

        if (wipe_disk) {
            if (p->size_mib == 0) fill_count++;
            else fixed_mib += p->size_mib;
        }
    }

    if (esp_count != 1) {
        snprintf(err, err_sz, "Exactly one EFI System Partition (/boot/efi) is required.");
        return false;
    }
    if (root_count != 1 || root_idx < 0) {
        snprintf(err, err_sz, "Exactly one Root (/) partition is required.");
        return false;
    }

    uint64_t rem_mib = 0;
    if (wipe_disk) {
        if (fill_count > 1) {
            snprintf(err, err_sz, "At most one partition may use remaining free space.");
            return false;
        }
        uint64_t disk_mib = disk_bytes / (1024ULL * 1024ULL);
        uint64_t usable_mib = (disk_mib > 2) ? (disk_mib - 2) : 0;
        if (fixed_mib > usable_mib) {
            snprintf(err, err_sz, "Total partition size (%llu MiB) exceeds usable disk (%llu MiB).",
                     (unsigned long long)fixed_mib, (unsigned long long)usable_mib);
            return false;
        }
        rem_mib = usable_mib - fixed_mib;
        if (fill_count == 1 && rem_mib < 64) {
            snprintf(err, err_sz, "Remaining space for fill partition is too small (%llu MiB).",
                     (unsigned long long)rem_mib);
            return false;
        }
    }

    if (min_root_bytes > 0) {
        uint64_t root_mib = (wipe_disk && parts[root_idx].size_mib == 0) ? rem_mib : parts[root_idx].size_mib;
        uint64_t min_root_mib = (min_root_bytes + (1024ULL * 1024ULL) - 1) / (1024ULL * 1024ULL);
        if (root_mib < min_root_mib) {
            snprintf(err, err_sz, "Root (/) is %llu MiB; this selection needs at least %llu MiB.",
                     (unsigned long long)root_mib, (unsigned long long)min_root_mib);
            return false;
        }
    }

    return true;
}

void disks_format_part_summary(const part_entry_t *parts, int count,
                               char *buf, size_t buf_sz) {
    if (!buf || buf_sz == 0) return;
    int active = 0;
    for (int i = 0; i < count; i++) {
        if (!parts[i].is_free && parts[i].role != PART_NONE) active++;
    }
    if (active <= 0) {
        snprintf(buf, buf_sz, "Default (ESP + /)");
        return;
    }
    size_t off = (size_t)snprintf(buf, buf_sz, "%d parts (", active);
    int printed = 0;
    for (int i = 0; i < count && off + 4 < buf_sz; i++) {
        if (parts[i].is_free || parts[i].role == PART_NONE) continue;
        const char *tag = (parts[i].role == PART_ESP) ? "ESP" : parts[i].mountpoint;
        char sz[24];
        if (parts[i].size_mib == 0) {
            snprintf(sz, sizeof(sz), "rest");
        } else if (parts[i].size_mib >= 1024 && (parts[i].size_mib % 1024) == 0) {
            snprintf(sz, sizeof(sz), "%lluG", (unsigned long long)(parts[i].size_mib / 1024));
        } else {
            snprintf(sz, sizeof(sz), "%lluM", (unsigned long long)parts[i].size_mib);
        }
        int n = snprintf(buf + off, buf_sz - off, "%s%s %s", (printed > 0) ? ", " : "", tag, sz);
        printed++;
        if (n > 0) off += (size_t)n;
        if (off >= buf_sz) {
            off = buf_sz - 1;
            break;
        }
    }
    if (off + 1 < buf_sz) {
        buf[off++] = ')';
        buf[off] = '\0';
    }
}

static void reread_part_table(const disk_t *d, install_progress_fn cb, int pct, void *ctx) {
    int fd = open(d->dev_path, O_RDONLY);
    if (fd >= 0) {
        ioctl(fd, 0x125F, 0); /* BLKRRPART */
        close(fd);
    }
    const char *blk_argv[] = {"blockdev", "--rereadpt", d->dev_path, NULL};
    run_cmd_logged(blk_argv, NULL, cb, pct, "Rereading partition table...", ctx);
}

static bool wait_for_part_nums(const disk_t *d, part_entry_t *parts, int count) {
    const char *p_sep = d->is_nvme ? "p" : "";
    for (int i = 0; i < count; i++) {
        if (parts[i].is_free || parts[i].role == PART_NONE) continue;
        int num = parts[i].part_num > 0 ? parts[i].part_num : (i + 1);
        snprintf(parts[i].dev_path, sizeof(parts[i].dev_path), "%s%s%d", d->dev_path, p_sep, num);
    }

    for (int retry = 0; retry < 30; retry++) {
        bool all_ok = true;
        for (int i = 0; i < count; i++) {
            if (parts[i].is_free || parts[i].role == PART_NONE) continue;
            if (access(parts[i].dev_path, F_OK) != 0) {
                all_ok = false;
                break;
            }
        }
        if (all_ok) return true;

        if (!d->is_nvme) {
            bool alt_ok = true;
            for (int i = 0; i < count; i++) {
                if (parts[i].is_free || parts[i].role == PART_NONE) continue;
                int num = parts[i].part_num > 0 ? parts[i].part_num : (i + 1);
                char alt[80];
                snprintf(alt, sizeof(alt), "%s%d", d->dev_path, num);
                if (access(alt, F_OK) != 0) {
                    snprintf(alt, sizeof(alt), "%sp%d", d->dev_path, num);
                    if (access(alt, F_OK) != 0) {
                        alt_ok = false;
                        break;
                    }
                }
            }
            if (alt_ok) {
                for (int i = 0; i < count; i++) {
                    if (parts[i].is_free || parts[i].role == PART_NONE) continue;
                    int num = parts[i].part_num > 0 ? parts[i].part_num : (i + 1);
                    char alt[80];
                    snprintf(alt, sizeof(alt), "%sp%d", d->dev_path, num);
                    if (access(alt, F_OK) == 0) {
                        snprintf(parts[i].dev_path, sizeof(parts[i].dev_path), "%s", alt);
                    }
                }
                return true;
            }
        }
        usleep(150000);
    }
    return false;
}

static int format_one_partition(const part_entry_t *p, int pct, install_progress_fn cb, void *ctx) {
    const char *lbl = p->label[0] ? p->label : "AvoryOS";
    if (p->fs == FS_FAT32) {
        if (cb) cb(pct, "Formatting EFI System Partition (FAT32)...", p->dev_path, ctx);
        const char *mk_vfat[] = {"mkfs.vfat", "-F", "32", "-n", lbl, p->dev_path, NULL};
        const char *bb_vfat[] = {"busybox", "mkfs.vfat", "-F", "32", "-n", lbl, p->dev_path, NULL};
        bool have_dosfstools = access("/usr/bin/mkfs.vfat", X_OK) == 0 || access("/usr/sbin/mkfs.vfat", X_OK) == 0;
        return run_cmd_logged(have_dosfstools ? mk_vfat : bb_vfat, NULL, cb, pct, "Formatting ESP...", ctx);
    }
    if (p->fs == FS_SWAP) {
        if (cb) cb(pct, "Formatting swap partition...", p->dev_path, ctx);
        const char *mk_swap[] = {"mkswap", "-L", lbl, p->dev_path, NULL};
        const char *bb_swap[] = {"busybox", "mkswap", p->dev_path, NULL};
        bool have_mkswap = access("/sbin/mkswap", X_OK) == 0 || access("/usr/sbin/mkswap", X_OK) == 0 ||
                           access("/bin/mkswap", X_OK) == 0 || access("/usr/bin/mkswap", X_OK) == 0;
        return run_cmd_logged(have_mkswap ? mk_swap : bb_swap, NULL, cb, pct, "Formatting swap...", ctx);
    }
    if (p->fs == FS_EXT4) {
        char stage[96];
        snprintf(stage, sizeof(stage), "Formatting %s (ext4)...", p->mountpoint);
        if (cb) cb(pct, stage, p->dev_path, ctx);
        /* Same layout as disk.img (GNUmakefile): the kernel's ext4 driver refuses
         * flex_bg, mounts metadata_csum/huge_file/dir_nlink/extra_isize read-only,
         * and its VFS has 32-bit file sizes, so large_file and 64bit stay off. */
        const char *mk_ext4[] = {
            "mke2fs", "-t", "ext4", "-F", "-L", lbl, "-b", "1024", "-I", "128",
            "-O", "extent,filetype,has_journal,dir_index,^64bit,^metadata_csum,^flex_bg,"
                  "^huge_file,^large_file,^dir_nlink,^extra_isize,^metadata_csum_seed,^orphan_file",
            p->dev_path, NULL
        };
        return run_cmd_logged(mk_ext4, NULL, cb, pct, stage, ctx);
    }
    return 0;
}

static void trim_trailing(char *s) {
    size_t len = strlen(s);
    while (len > 0 && (s[len - 1] == '\n' || s[len - 1] == '\r' || s[len - 1] == ' ')) {
        s[--len] = '\0';
    }
}

static void probe_blkid_tag(const char *dev_path, const char *tag, char *out, size_t out_sz) {
    const char *argv[] = {"blkid", "-s", tag, "-o", "value", dev_path, NULL};
    if (run_cmd_capture(argv, out, out_sz) == 0) {
        trim_trailing(out);
    } else {
        out[0] = '\0';
    }
}

int disks_scan_partitions(const disk_t *d, part_entry_t *out, int max_parts) {
    const char *argv[] = {"parted", "-s", "-m", d->dev_path, "unit", "MiB", "print", "free", NULL};
    char buf[8192] = {0};
    if (run_cmd_capture(argv, buf, sizeof(buf)) != 0) return 0;

    int count = 0;
    const char *p_sep = d->is_nvme ? "p" : "";
    char *save = NULL;
    for (char *line = strtok_r(buf, "\n", &save); line && count < max_parts; line = strtok_r(NULL, "\n", &save)) {
        if (strncmp(line, "BYT;", 4) == 0 || strncmp(line, "/dev/", 5) == 0) continue;
        size_t l = strlen(line);
        if (l > 0 && line[l - 1] == ';') line[l - 1] = '\0';

        char *fields[8] = {0};
        int nf = 0;
        char *cur = line;
        while (nf < 8) {
            fields[nf++] = cur;
            char *colon = strchr(cur, ':');
            if (!colon) break;
            *colon = '\0';
            cur = colon + 1;
        }
        if (nf < 5) continue;

        uint64_t start_mib = strtoull(fields[1], NULL, 10);
        uint64_t end_mib = strtoull(fields[2], NULL, 10);
        uint64_t size_mib = (end_mib > start_mib) ? (end_mib - start_mib) : strtoull(fields[3], NULL, 10);
        const char *fs_str = fields[4];

        uint64_t disk_mib = d->size_bytes / (1024ULL * 1024ULL);
        uint64_t max_end_mib = (disk_mib > 1) ? (disk_mib - 1) : end_mib;
        if (strcmp(fs_str, "free") == 0) {
            if (end_mib > max_end_mib) end_mib = max_end_mib;
            size_mib = (end_mib > start_mib) ? (end_mib - start_mib) : 0;
            if (size_mib < 16) continue; /* skip tiny GPT alignment gaps */
            part_entry_t *p = &out[count++];
            memset(p, 0, sizeof(*p));
            p->is_free = true;
            p->role = PART_NONE;
            p->fs = FS_OTHER;
            p->start_mib = start_mib;
            p->size_mib = size_mib;
            snprintf(p->cur_fs, sizeof(p->cur_fs), "free");
            snprintf(p->mountpoint, sizeof(p->mountpoint), "[FREE SPACE]");
            continue;
        }

        int pnum = atoi(fields[0]);
        if (pnum <= 0) continue;

        part_entry_t *p = &out[count++];
        memset(p, 0, sizeof(*p));
        p->existing = true;
        p->part_num = pnum;
        p->role = PART_NONE;
        p->fs = FS_OTHER;
        p->start_mib = start_mib;
        p->size_mib = size_mib;
        snprintf(p->dev_path, sizeof(p->dev_path), "%s%s%d", d->dev_path, p_sep, pnum);
        snprintf(p->mountpoint, sizeof(p->mountpoint), "[ignore]");
        if (nf >= 6 && fields[5][0]) {
            snprintf(p->label, sizeof(p->label), "%s", fields[5]);
        }
        if (fs_str[0]) {
            snprintf(p->cur_fs, sizeof(p->cur_fs), "%s", fs_str);
        } else {
            probe_blkid_tag(p->dev_path, "TYPE", p->cur_fs, sizeof(p->cur_fs));
        }
        if (!p->label[0]) {
            probe_blkid_tag(p->dev_path, "LABEL", p->label, sizeof(p->label));
        }
        if (!p->cur_fs[0]) snprintf(p->cur_fs, sizeof(p->cur_fs), "unknown");
    }
    return count;
}

int disks_delete_partition(const disk_t *d, int part_num) {
    char num_s[16];
    snprintf(num_s, sizeof(num_s), "%d", part_num);
    const char *argv[] = {"parted", "-s", d->dev_path, "rm", num_s, NULL};
    if (run_cmd_logged(argv, NULL, NULL, 0, NULL, NULL) != 0) return -1;
    reread_part_table(d, NULL, 0, NULL);
    return 0;
}

int disks_shrink_partition(const disk_t *d, const part_entry_t *p,
                           uint64_t new_size_mib, char *err, size_t err_sz) {
    if (!p || !p->existing || p->part_num <= 0) {
        snprintf(err, err_sz, "Only existing partitions can be shrunk.");
        return -1;
    }
    if (new_size_mib < 256 || new_size_mib + 64 > p->size_mib) {
        snprintf(err, err_sz, "New size must be between 256 MiB and %llu MiB.",
                 (unsigned long long)(p->size_mib > 64 ? p->size_mib - 64 : 0));
        return -1;
    }

    char sz_arg[32], pnum_s[16], end_s[32];
    snprintf(sz_arg, sizeof(sz_arg), "%lluM", (unsigned long long)new_size_mib);
    snprintf(pnum_s, sizeof(pnum_s), "%d", p->part_num);
    snprintf(end_s, sizeof(end_s), "%lluMiB", (unsigned long long)(p->start_mib + new_size_mib));

    if (strncmp(p->cur_fs, "ext", 3) == 0) {
        const char *fsck_argv[] = {"e2fsck", "-f", "-p", p->dev_path, NULL};
        int frc = run_cmd_logged(fsck_argv, NULL, NULL, 0, NULL, NULL);
        if (frc != 0 && frc != 1) {
            snprintf(err, err_sz, "e2fsck failed on %s (exit %d).", p->dev_path, frc);
            return -1;
        }
        const char *rs_argv[] = {"resize2fs", p->dev_path, sz_arg, NULL};
        if (run_cmd_logged(rs_argv, NULL, NULL, 0, NULL, NULL) != 0) {
            snprintf(err, err_sz, "resize2fs failed on %s (is target size smaller than used data?).", p->dev_path);
            return -1;
        }
    } else if (strcmp(p->cur_fs, "ntfs") == 0) {
        if (access("/usr/bin/ntfsresize", X_OK) != 0 && access("/usr/sbin/ntfsresize", X_OK) != 0) {
            snprintf(err, err_sz, "ntfsresize is not installed; shrink NTFS from Windows or GParted.");
            return -1;
        }
        const char *ntfs_argv[] = {"ntfsresize", "-f", "-s", sz_arg, p->dev_path, NULL};
        if (run_cmd_logged(ntfs_argv, NULL, NULL, 0, NULL, NULL) != 0) {
            snprintf(err, err_sz, "ntfsresize failed on %s.", p->dev_path);
            return -1;
        }
    } else {
        snprintf(err, err_sz, "Shrinking '%s' filesystems is not supported.", p->cur_fs);
        return -1;
    }

    const char *rp_argv[] = {"parted", "---pretend-input-tty", d->dev_path,
                             "resizepart", pnum_s, end_s, NULL};
    if (run_cmd_logged(rp_argv, "Yes\n", NULL, 0, NULL, NULL) != 0) {
        snprintf(err, err_sz, "parted resizepart failed on %s.", p->dev_path);
        return -1;
    }
    reread_part_table(d, NULL, 0, NULL);
    return 0;
}

int disks_partition_and_format(const disk_t *d,
                               part_entry_t *parts, int count,
                               install_progress_fn cb, void *ctx) {
    install_unmount_target();

    if (cb) cb(10, "Creating GPT partition table...", NULL, ctx);

    const char *p_mklabel[] = {"parted", "-s", d->dev_path, "mklabel", "gpt", NULL};
    if (run_cmd_logged(p_mklabel, NULL, cb, 12, "Creating GPT label...", ctx) != 0) return -1;

    uint64_t disk_mib = d->size_bytes / (1024ULL * 1024ULL);
    uint64_t usable_mib = (disk_mib > 2) ? (disk_mib - 2) : 0;
    uint64_t fixed_mib = 0;
    for (int i = 0; i < count; i++) fixed_mib += parts[i].size_mib;
    uint64_t rem_mib = (usable_mib > fixed_mib) ? (usable_mib - fixed_mib) : 0;

    uint64_t start_mib = 1;
    for (int i = 0; i < count; i++) {
        part_entry_t *p = &parts[i];
        p->part_num = i + 1;
        char start_s[32], end_s[32], part_num_s[16], stage[96];
        snprintf(start_s, sizeof(start_s), "%lluMiB", (unsigned long long)start_mib);

        uint64_t sz = (p->size_mib == 0) ? rem_mib : p->size_mib;
        if (p->size_mib == 0 && i == count - 1) {
            snprintf(end_s, sizeof(end_s), "100%%");
        } else {
            snprintf(end_s, sizeof(end_s), "%lluMiB", (unsigned long long)(start_mib + sz));
        }
        start_mib += sz;

        const char *parted_fs = (p->fs == FS_FAT32) ? "fat32" :
                                (p->fs == FS_SWAP) ? "linux-swap" : "ext4";
        const char *lbl = p->label[0] ? p->label : (p->role == PART_ESP ? "EFI" : "AvoryOS");

        int pct = 14 + (i * 8) / (count > 0 ? count : 1);
        snprintf(stage, sizeof(stage), "Creating partition %d (%s)...", i + 1, p->mountpoint);
        const char *p_mkpart[] = {"parted", "-s", d->dev_path, "mkpart", lbl, parted_fs, start_s, end_s, NULL};
        if (run_cmd_logged(p_mkpart, NULL, cb, pct, stage, ctx) != 0) return -1;

        if (p->role == PART_ESP) {
            snprintf(part_num_s, sizeof(part_num_s), "%d", i + 1);
            const char *p_flag[] = {"parted", "-s", d->dev_path, "set", part_num_s, "esp", "on", NULL};
            if (run_cmd_logged(p_flag, NULL, cb, pct + 1, "Setting ESP flag...", ctx) != 0) return -1;
        }
    }

    reread_part_table(d, cb, 22, ctx);

    if (!wait_for_part_nums(d, parts, count)) {
        if (cb) cb(22, "Timed out waiting for partition device nodes", d->dev_path, ctx);
        return -1;
    }

    for (int i = 0; i < count; i++) {
        int pct = 25 + ((i + 1) * 15) / (count > 0 ? count : 1);
        if (format_one_partition(&parts[i], pct, cb, ctx) != 0) return -1;
    }

    return 0;
}

int disks_apply_existing(const disk_t *d,
                         part_entry_t *parts, int count,
                         install_progress_fn cb, void *ctx) {
    install_unmount_target();
    if (cb) cb(10, "Applying partition changes...", NULL, ctx);

    for (int i = 0; i < count; i++) {
        part_entry_t *p = &parts[i];
        if (p->is_free || p->role == PART_NONE || p->existing) continue;

        uint64_t disk_mib = d->size_bytes / (1024ULL * 1024ULL);
        uint64_t end_mib = p->start_mib + p->size_mib;
        if (disk_mib > 1 && end_mib > disk_mib - 1) end_mib = disk_mib - 1;
        char start_s[32], end_s[32], stage[96];
        snprintf(start_s, sizeof(start_s), "%lluMiB", (unsigned long long)p->start_mib);
        snprintf(end_s, sizeof(end_s), "%lluMiB", (unsigned long long)end_mib);
        const char *parted_fs = (p->fs == FS_FAT32) ? "fat32" :
                                (p->fs == FS_SWAP) ? "linux-swap" : "ext4";
        const char *lbl = p->label[0] ? p->label : (p->role == PART_ESP ? "EFI" : "AvoryOS");

        snprintf(stage, sizeof(stage), "Creating partition (%s) in free space...", p->mountpoint);
        const char *p_mkpart[] = {"parted", "-s", d->dev_path, "mkpart", lbl, parted_fs, start_s, end_s, NULL};
        if (run_cmd_logged(p_mkpart, NULL, cb, 16, stage, ctx) != 0) return -1;

        /* Discover the assigned partition number by matching start_mib */
        part_entry_t scanned[MAX_PARTITIONS];
        int nscan = disks_scan_partitions(d, scanned, MAX_PARTITIONS);
        for (int j = 0; j < nscan; j++) {
            if (!scanned[j].existing) continue;
            uint64_t diff = (scanned[j].start_mib > p->start_mib)
                            ? (scanned[j].start_mib - p->start_mib)
                            : (p->start_mib - scanned[j].start_mib);
            if (diff <= 2) {
                p->part_num = scanned[j].part_num;
                snprintf(p->dev_path, sizeof(p->dev_path), "%s", scanned[j].dev_path);
                break;
            }
        }
        if (p->part_num <= 0) return -1;

        if (p->role == PART_ESP) {
            char pnum_s[16];
            snprintf(pnum_s, sizeof(pnum_s), "%d", p->part_num);
            const char *p_flag[] = {"parted", "-s", d->dev_path, "set", pnum_s, "esp", "on", NULL};
            if (run_cmd_logged(p_flag, NULL, cb, 18, "Setting ESP flag...", ctx) != 0) return -1;
        }
    }

    reread_part_table(d, cb, 22, ctx);

    if (!wait_for_part_nums(d, parts, count)) {
        if (cb) cb(22, "Timed out waiting for partition device nodes", d->dev_path, ctx);
        return -1;
    }

    for (int i = 0; i < count; i++) {
        part_entry_t *p = &parts[i];
        if (p->is_free || p->role == PART_NONE) continue;
        if (p->existing && !p->format) continue;
        int pct = 25 + ((i + 1) * 15) / (count > 0 ? count : 1);
        if (format_one_partition(p, pct, cb, ctx) != 0) return -1;
    }

    return 0;
}

bool disks_get_uuid(const char *dev_path, char *out_uuid, size_t uuid_sz) {
    const char *argv[] = {"blkid", "-s", "UUID", "-o", "value", dev_path, NULL};
    char buf[128] = {0};
    if (run_cmd_capture(argv, buf, sizeof(buf)) != 0) return false;
    size_t len = strlen(buf);
    while (len > 0 && (buf[len - 1] == '\n' || buf[len - 1] == '\r' || buf[len - 1] == ' ')) {
        buf[--len] = '\0';
    }
    if (len == 0) return false;
    snprintf(out_uuid, uuid_sz, "%s", buf);
    return true;
}
