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

int disks_scan(disk_t *out, int max_disks) {
    int count = 0;
    DIR *dir = opendir("/sys/block");
    if (!dir) return 0;

    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL && count < max_disks) {
        if (ent->d_name[0] == '.') continue;
        if (!is_whole_disk_name(ent->d_name)) continue;

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
            snprintf(d->model, sizeof(d->model), "%s", d->is_nvme ? "NVMe Disk" : "Block Disk");
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

static bool wait_for_partitions(const disk_t *d, char *out_esp, size_t esp_sz, char *out_root, size_t root_sz) {
    const char *p_sep = d->is_nvme ? "p" : "";
    snprintf(out_esp, esp_sz, "%s%s1", d->dev_path, p_sep);
    snprintf(out_root, root_sz, "%s%s2", d->dev_path, p_sep);

    for (int retry = 0; retry < 30; retry++) {
        if (access(out_esp, F_OK) == 0 && access(out_root, F_OK) == 0) {
            return true;
        }
        if (!d->is_nvme) {
            char alt_esp[72], alt_root[72];
            snprintf(alt_esp, sizeof(alt_esp), "%sp1", d->dev_path);
            snprintf(alt_root, sizeof(alt_root), "%sp2", d->dev_path);
            if (access(alt_esp, F_OK) == 0 && access(alt_root, F_OK) == 0) {
                snprintf(out_esp, esp_sz, "%s", alt_esp);
                snprintf(out_root, root_sz, "%s", alt_root);
                return true;
            }
        }
        usleep(150000);
    }
    return false;
}

int disks_partition_and_format(const disk_t *d,
                               char *out_esp, size_t esp_sz,
                               char *out_root, size_t root_sz,
                               install_progress_fn cb, void *ctx) {
    umount2(TARGET_MOUNT "/boot/efi", 2);
    umount2(TARGET_MOUNT, 2);

    if (cb) cb(10, "Creating GPT partition table...", NULL, ctx);

    const char *p_mklabel[] = {"parted", "-s", d->dev_path, "mklabel", "gpt", NULL};
    if (run_cmd_logged(p_mklabel, NULL, cb, 12, "Creating GPT label...", ctx) != 0) return -1;

    const char *p_esp[] = {"parted", "-s", d->dev_path, "mkpart", "EFI", "fat32", "1MiB", "129MiB", NULL};
    if (run_cmd_logged(p_esp, NULL, cb, 15, "Creating EFI System Partition...", ctx) != 0) return -1;

    const char *p_flag[] = {"parted", "-s", d->dev_path, "set", "1", "esp", "on", NULL};
    if (run_cmd_logged(p_flag, NULL, cb, 17, "Setting ESP flag...", ctx) != 0) return -1;

    const char *p_root[] = {"parted", "-s", d->dev_path, "mkpart", "AvoryOS-Root", "ext4", "129MiB", "100%", NULL};
    if (run_cmd_logged(p_root, NULL, cb, 20, "Creating AvoryOS-Root partition...", ctx) != 0) return -1;

    int fd = open(d->dev_path, O_RDONLY);
    if (fd >= 0) {
        ioctl(fd, 0x125F, 0); /* BLKRRPART */
        close(fd);
    }
    const char *blk_argv[] = {"blockdev", "--rereadpt", d->dev_path, NULL};
    run_cmd_logged(blk_argv, NULL, cb, 22, "Rereading partition table...", ctx);

    if (!wait_for_partitions(d, out_esp, esp_sz, out_root, root_sz)) {
        if (cb) cb(22, "Timed out waiting for partition device nodes", out_root, ctx);
        return -1;
    }

    if (cb) cb(25, "Formatting EFI System Partition (FAT32)...", out_esp, ctx);
    /* dosfstools may be missing from older live images; busybox has an
     * mkfs.vfat applet with the same options. */
    const char *mk_vfat[] = {"mkfs.vfat", "-F", "32", "-n", "EFI", out_esp, NULL};
    const char *bb_vfat[] = {"busybox", "mkfs.vfat", "-F", "32", "-n", "EFI", out_esp, NULL};
    bool have_dosfstools = access("/usr/bin/mkfs.vfat", X_OK) == 0 || access("/usr/sbin/mkfs.vfat", X_OK) == 0;
    if (run_cmd_logged(have_dosfstools ? mk_vfat : bb_vfat, NULL, cb, 30, "Formatting ESP...", ctx) != 0) return -1;

    if (cb) cb(35, "Formatting root partition (ext4)...", out_root, ctx);
    /* Same layout as disk.img (GNUmakefile): the kernel's ext4 driver refuses
     * flex_bg, mounts metadata_csum/huge_file/dir_nlink/extra_isize read-only,
     * and its VFS has 32-bit file sizes, so large_file and 64bit stay off. */
    const char *mk_ext4[] = {
        "mke2fs", "-t", "ext4", "-F", "-L", "AvoryOS-Root", "-b", "1024", "-I", "128",
        "-O", "extent,filetype,has_journal,dir_index,^64bit,^metadata_csum,^flex_bg,"
              "^huge_file,^large_file,^dir_nlink,^extra_isize,^metadata_csum_seed,^orphan_file",
        out_root, NULL
    };
    if (run_cmd_logged(mk_ext4, NULL, cb, 40, "Formatting ext4 root...", ctx) != 0) return -1;

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
