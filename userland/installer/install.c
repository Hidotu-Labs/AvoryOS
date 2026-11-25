#include "install.h"
#include "disks.h"
#include "packages.h"
#include "tui.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

void install_log(const char *line) {
    FILE *f = fopen(LOG_PATH, "a");
    if (!f) return;
    fputs(line, f);
    size_t len = strlen(line);
    if (len == 0 || line[len - 1] != '\n') fputc('\n', f);
    fclose(f);
}

static int ensure_dir(const char *path, mode_t mode) {
    char tmp[512];
    snprintf(tmp, sizeof(tmp), "%s", path);
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            mkdir(tmp, mode);
            *p = '/';
        }
    }
    if (mkdir(tmp, mode) != 0 && errno != EEXIST) return -1;
    return 0;
}

static int copy_file(const char *src, const char *dst, mode_t mode) {
    int in_fd = open(src, O_RDONLY);
    if (in_fd < 0) return -1;
    int out_fd = open(dst, O_WRONLY | O_CREAT | O_TRUNC, mode);
    if (out_fd < 0) {
        close(in_fd);
        return -1;
    }
    char buf[16384];
    ssize_t n;
    while ((n = read(in_fd, buf, sizeof(buf))) > 0) {
        ssize_t off = 0;
        while (off < n) {
            ssize_t w = write(out_fd, buf + off, (size_t)(n - off));
            if (w <= 0) {
                close(in_fd);
                close(out_fd);
                return -1;
            }
            off += w;
        }
    }
    close(in_fd);
    close(out_fd);
    return n < 0 ? -1 : 0;
}

/* Apply \b and \r the way a terminal would, so progress counters such as
 * mke2fs's "0/261\b\b\b\b\b" end up as their final text. */
static void flatten_progress(char *s) {
    size_t out = 0;
    for (size_t i = 0; s[i]; i++) {
        if (s[i] == '\r') out = 0;
        else if (s[i] == '\b') { if (out > 0) out--; }
        else s[out++] = s[i];
    }
    s[out] = '\0';
}

int run_cmd_logged(const char *const argv[], const char *stdin_data,
                   install_progress_fn cb, int pct, const char *stage, void *ctx) {
    /* Log command */
    char cmdline[512] = "$";
    for (int i = 0; argv[i]; i++) {
        strncat(cmdline, " ", sizeof(cmdline) - strlen(cmdline) - 1);
        strncat(cmdline, argv[i], sizeof(cmdline) - strlen(cmdline) - 1);
    }
    install_log(cmdline);
    if (cb) cb(pct, stage, cmdline, ctx);

    int out_pipe[2];
    int in_pipe[2] = {-1, -1};
    if (pipe(out_pipe) != 0) return -1;
    if (stdin_data && pipe(in_pipe) != 0) {
        close(out_pipe[0]);
        close(out_pipe[1]);
        return -1;
    }

    pid_t pid = fork();
    if (pid == 0) {
        close(out_pipe[0]);
        dup2(out_pipe[1], STDOUT_FILENO);
        dup2(out_pipe[1], STDERR_FILENO);
        close(out_pipe[1]);

        if (stdin_data) {
            close(in_pipe[1]);
            dup2(in_pipe[0], STDIN_FILENO);
            close(in_pipe[0]);
        } else {
            int devnull = open("/dev/null", O_RDONLY);
            if (devnull >= 0) {
                dup2(devnull, STDIN_FILENO);
                close(devnull);
            }
        }

        execvp(argv[0], (char *const *)argv);
        fprintf(stderr, "execvp(%s) failed: %s\n", argv[0], strerror(errno));
        _exit(127);
    }

    close(out_pipe[1]);
    if (stdin_data) {
        close(in_pipe[0]);
        size_t len = strlen(stdin_data);
        size_t off = 0;
        while (off < len) {
            ssize_t w = write(in_pipe[1], stdin_data + off, len - off);
            if (w <= 0) break;
            off += (size_t)w;
        }
        close(in_pipe[1]);
    }

    FILE *fp = fdopen(out_pipe[0], "r");
    if (fp) {
        char line[512];
        while (fgets(line, sizeof(line), fp)) {
            size_t l = strlen(line);
            while (l > 0 && (line[l - 1] == '\n' || line[l - 1] == '\r')) {
                line[--l] = '\0';
            }
            flatten_progress(line);
            l = strlen(line);
            if (l > 0) {
                install_log(line);
                if (cb) cb(pct, stage, line, ctx);
            }
        }
        fclose(fp);
    } else {
        close(out_pipe[0]);
    }

    int status = 0;
    if (waitpid(pid, &status, 0) < 0) return -1;
    int rc = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    if (rc != 0) {
        char err[128];
        snprintf(err, sizeof(err), "[exit %d] %s", rc, argv[0]);
        install_log(err);
    }
    return rc;
}

int run_cmd_capture(const char *const argv[], char *out, size_t out_sz) {
    if (!out || out_sz == 0) return -1;
    out[0] = '\0';

    int p[2];
    if (pipe(p) != 0) return -1;

    pid_t pid = fork();
    if (pid == 0) {
        close(p[0]);
        dup2(p[1], STDOUT_FILENO);
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) {
            dup2(devnull, STDERR_FILENO);
            close(devnull);
        }
        close(p[1]);
        execvp(argv[0], (char *const *)argv);
        _exit(127);
    }
    close(p[1]);

    size_t total = 0;
    while (total + 1 < out_sz) {
        ssize_t n = read(p[0], out + total, out_sz - 1 - total);
        if (n <= 0) break;
        total += (size_t)n;
    }
    out[total] = '\0';
    close(p[0]);

    int status = 0;
    if (waitpid(pid, &status, 0) < 0) return -1;
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

void install_unmount_target(void) {
    umount2(TARGET_MOUNT "/dev", 2);
    umount2(TARGET_MOUNT "/proc", 2);
    umount2(TARGET_MOUNT "/sys", 2);

    /* Unmount any nested mountpoints under TARGET_MOUNT deepest-first */
    char submounts[16][128];
    int n_sub = 0;
    size_t base_len = strlen(TARGET_MOUNT);
    FILE *m = fopen("/proc/mounts", "r");
    if (m) {
        char line[512];
        while (fgets(line, sizeof(line), m) && n_sub < 16) {
            char mdev[128] = {0}, mp[128] = {0};
            if (sscanf(line, "%127s %127s", mdev, mp) == 2) {
                if (strncmp(mp, TARGET_MOUNT "/", base_len + 1) == 0) {
                    snprintf(submounts[n_sub++], sizeof(submounts[0]), "%s", mp);
                }
            }
        }
        fclose(m);
    }
    for (int i = 0; i < n_sub - 1; i++) {
        for (int j = i + 1; j < n_sub; j++) {
            if (strlen(submounts[j]) > strlen(submounts[i])) {
                char tmp[128];
                memcpy(tmp, submounts[i], sizeof(tmp));
                memcpy(submounts[i], submounts[j], sizeof(submounts[i]));
                memcpy(submounts[j], tmp, sizeof(submounts[j]));
            }
        }
    }
    for (int i = 0; i < n_sub; i++) {
        umount2(submounts[i], 2);
    }

    umount2(TARGET_MOUNT "/boot/efi", 2);
    umount2(TARGET_MOUNT, 2);
}

static void set_account_password(const char *user, const char *password,
                                 install_progress_fn cb, int pct,
                                 const char *stage, void *ctx) {
    char pw_in[128];
    snprintf(pw_in, sizeof(pw_in), "%s:%s\n", user, password);

    const char *chroot_chpw[] = {"chroot", TARGET_MOUNT, "/usr/bin/chpasswd", NULL};
    if (run_cmd_logged(chroot_chpw, pw_in, cb, pct, stage, ctx) == 0) {
        return;
    }

    const char *root_chpw[] = {"chpasswd", "--root", TARGET_MOUNT, NULL};
    if (run_cmd_logged(root_chpw, pw_in, cb, pct, stage, ctx) == 0) {
        return;
    }

    /* Fallback when PAM/chpasswd fails in the live chroot: hash with openssl
     * and update /etc/shadow + /etc/passwd directly. */
    const char *cmd =
        "u=\"$1\"; p=\"$2\"; "
        "h=$(printf '%s\\n' \"$p\" | /usr/bin/openssl passwd -6 -stdin 2>/dev/null) || exit 1; "
        "sed -i -E \"s|^(${u}):[^:]*:|\\1:x:|\" /etc/passwd; "
        "if [ -f /etc/shadow ] && grep -q \"^${u}:\" /etc/shadow; then "
        "  awk -F: -v OFS=: -v u=\"$u\" -v h=\"$h\" '$1==u{$2=h} {print}' /etc/shadow > /etc/shadow.tmp && "
        "  chmod 0000 /etc/shadow.tmp && mv /etc/shadow.tmp /etc/shadow; "
        "else "
        "  printf '%s:%s:19000:0:99999:7:::\\n' \"$u\" \"$h\" >> /etc/shadow; "
        "fi";
    const char *fallback_argv[] = {"chroot", TARGET_MOUNT, "/bin/sh", "-c", cmd, "sh", user, password, NULL};
    run_cmd_logged(fallback_argv, NULL, cb, pct, stage, ctx);
}

void install_enter_chroot(void) {
    ensure_dir(TARGET_MOUNT "/dev", 0755);
    ensure_dir(TARGET_MOUNT "/proc", 0755);
    ensure_dir(TARGET_MOUNT "/sys", 0755);

    mount("/dev", TARGET_MOUNT "/dev", NULL, MS_BIND, NULL);
    mount("/proc", TARGET_MOUNT "/proc", "proc", 0, NULL);
    mount("/sys", TARGET_MOUNT "/sys", "sysfs", 0, NULL);

    pid_t pid = fork();
    if (pid == 0) {
        const char *sh = access(TARGET_MOUNT "/bin/bash", X_OK) == 0 ? "/bin/bash" : "/bin/sh";
        const char *argv[] = {"chroot", TARGET_MOUNT, sh, NULL};
        execvp("chroot", (char *const *)argv);
        _exit(127);
    }
    if (pid > 0) {
        int st = 0;
        waitpid(pid, &st, 0);
    }

    umount2(TARGET_MOUNT "/sys", 2);
    umount2(TARGET_MOUNT "/proc", 2);
    umount2(TARGET_MOUNT "/dev", 2);
}

static int write_xinitrc(const char *path, const char *xinit_cmd, const char *xkb_layout) {
    const char *xdg_env =
        "_avory_home=\"${HOME:-/}\"\n"
        "_avory_home=\"${_avory_home%/}\"\n"
        "export XDG_CONFIG_HOME=\"${XDG_CONFIG_HOME:-${_avory_home}/.config}\"\n"
        "export XDG_CACHE_HOME=\"${XDG_CACHE_HOME:-${_avory_home}/.cache}\"\n"
        "export XDG_DATA_HOME=\"${XDG_DATA_HOME:-${_avory_home}/.local/share}\"\n"
        "export XDG_STATE_HOME=\"${XDG_STATE_HOME:-${_avory_home}/.local/state}\"\n"
        "unset _avory_home\n";

    FILE *f = fopen(path, "w");
    if (!f) return -1;
    fputs("#!/bin/sh\n", f);
    fputs(xdg_env, f);
    if (xkb_layout && xkb_layout[0]) {
        fprintf(f, "command -v setxkbmap >/dev/null 2>&1 && setxkbmap %s 2>/dev/null || true\n", xkb_layout);
    }
    fputs("_xfce_log=\"/tmp/xfce4-session-$(id -u).log\"\n"
          "if [ -n \"${DBUS_SESSION_BUS_ADDRESS:-}\" ] && command -v dbus-update-activation-environment >/dev/null 2>&1; then\n"
          "    dbus-update-activation-environment --all 2>/dev/null || true\n"
          "fi\n", f);
    if (strcmp(xinit_cmd, "exec startxfce4") == 0) {
        fputs("if [ -z \"${DBUS_SESSION_BUS_ADDRESS:-}\" ] && command -v dbus-run-session >/dev/null 2>&1; then\n"
              "    exec dbus-run-session -- startxfce4 >>\"$_xfce_log\" 2>&1\n"
              "fi\n"
              "exec startxfce4 >>\"$_xfce_log\" 2>&1\n", f);
    } else {
        fprintf(f, "%s\n", xinit_cmd);
    }
    fclose(f);
    chmod(path, 0755);
    return 0;
}

int install_system(const installer_cfg_t *cfg, const disk_t *disk,
                   install_progress_fn cb, void *ctx,
                   char *err_buf, size_t err_sz) {
    /* Truncate log file */
    FILE *lf = fopen(LOG_PATH, "w");
    if (lf) {
        fprintf(lf, "AvoryOS Installer v%s\nTarget: %s\n", INSTALLER_VERSION, disk->dev_path);
        fclose(lf);
    }

    if (cfg->dry_run) {
        const char *stages[] = {
            "Dry-run: would partition GPT + ESP + ext4",
            "Dry-run: would format FAT32 + ext4",
            "Dry-run: would copy selected packages",
            "Dry-run: would install Limine & kernel",
            "Dry-run: would configure fstab, keymap, users",
        };
        for (int i = 0; i < 5; i++) {
            if (cb) cb(20 * (i + 1), stages[i], stages[i], ctx);
            usleep(350000);
        }
        return 0;
    }

    if (cb) cb(5, "Resolving packages...", NULL, ctx);
    pkgsel_t *sel = pkgsel_load(cfg, err_buf, err_sz);
    if (!sel) return -1;

    /* ESP, filesystem metadata, firmware and unpackaged files need room on
     * top of the packages themselves. */
    uint64_t need = pkgsel_installed_bytes(sel) + pkgsel_installed_bytes(sel) / 4 + (1ULL << 30);

    part_entry_t parts[MAX_PARTITIONS];
    int part_count = cfg->part_count;
    bool wipe_disk = cfg->wipe_disk;
    if (part_count > 0 && part_count <= MAX_PARTITIONS) {
        memcpy(parts, cfg->parts, (size_t)part_count * sizeof(part_entry_t));
    } else {
        wipe_disk = true;
        disks_init_preset(SCHEME_SINGLE_ROOT, disk->size_bytes, parts, &part_count);
    }

    if (!disks_validate_parts(parts, part_count, disk->size_bytes, need, wipe_disk, err_buf, err_sz)) {
        pkgsel_free(sel);
        return -1;
    }

    int part_rc = wipe_disk
        ? disks_partition_and_format(disk, parts, part_count, cb, ctx)
        : disks_apply_existing(disk, parts, part_count, cb, ctx);
    if (part_rc != 0) {
        snprintf(err_buf, err_sz, "Partitioning or formatting %s failed. See %s", disk->dev_path, LOG_PATH);
        pkgsel_free(sel);
        return -1;
    }

    char esp_dev[80] = {0};
    char root_dev[80] = {0};
    for (int i = 0; i < part_count; i++) {
        if (parts[i].is_free || parts[i].role == PART_NONE) continue;
        if (parts[i].role == PART_ESP) {
            snprintf(esp_dev, sizeof(esp_dev), "%s", parts[i].dev_path);
        } else if (parts[i].role == PART_ROOT || strcmp(parts[i].mountpoint, "/") == 0) {
            snprintf(root_dev, sizeof(root_dev), "%s", parts[i].dev_path);
        }
    }

    /* Mount root */
    if (cb) cb(42, "Mounting target root filesystem...", root_dev, ctx);
    ensure_dir(TARGET_MOUNT, 0755);
    if (mount(root_dev, TARGET_MOUNT, "ext4", 0, NULL) != 0) {
        const char *m_argv[] = {"mount", "-t", "ext4", root_dev, TARGET_MOUNT, NULL};
        if (run_cmd_logged(m_argv, NULL, cb, 42, "Mounting root...", ctx) != 0) {
            snprintf(err_buf, err_sz, "Failed to mount %s on %s", root_dev, TARGET_MOUNT);
            pkgsel_free(sel);
            return -1;
        }
    }

    /* Mount additional ext4 partitions shallowest-first before copying files */
    int extra_idx[MAX_PARTITIONS];
    int n_extra = 0;
    for (int i = 0; i < part_count; i++) {
        if (parts[i].is_free || parts[i].role == PART_NONE) continue;
        if (parts[i].fs == FS_EXT4 && parts[i].role != PART_ROOT && strcmp(parts[i].mountpoint, "/") != 0) {
            extra_idx[n_extra++] = i;
        }
    }
    for (int i = 0; i < n_extra - 1; i++) {
        for (int j = i + 1; j < n_extra; j++) {
            if (strlen(parts[extra_idx[j]].mountpoint) < strlen(parts[extra_idx[i]].mountpoint)) {
                int tmp = extra_idx[i];
                extra_idx[i] = extra_idx[j];
                extra_idx[j] = tmp;
            }
        }
    }
    for (int k = 0; k < n_extra; k++) {
        part_entry_t *p = &parts[extra_idx[k]];
        char mpath[192];
        snprintf(mpath, sizeof(mpath), TARGET_MOUNT "%s", p->mountpoint);
        ensure_dir(mpath, 0755);
        if (cb) cb(44, "Mounting additional partition...", p->mountpoint, ctx);
        const char *m_argv[] = {"mount", "-t", "ext4", p->dev_path, mpath, NULL};
        if (run_cmd_logged(m_argv, NULL, cb, 44, "Mounting partition...", ctx) != 0) {
            snprintf(err_buf, err_sz, "Failed to mount %s on %s", p->dev_path, mpath);
            install_unmount_target();
            pkgsel_free(sel);
            return -1;
        }
    }

    /* Copy the selected packages and AvoryOS's own files */
    /* The copy runs in a child that exits without freeing its multi-MiB
     * tables: a crash there (seen as heap corruption at teardown on AvoryOS)
     * then fails the install with a message instead of killing the TUI, and
     * a finished copy no longer depends on free() succeeding. The selection
     * itself is left for exit to reclaim for the same reason. */
    pid_t copier = fork();
    if (copier == 0) {
        _exit(pkgsel_copy(sel, cb, ctx, 46, 78, err_buf, err_sz) == 0 ? 0 : 1);
    }
    int copy_status = 0;
    bool copy_ok = copier > 0;
    if (copy_ok) {
        while (waitpid(copier, &copy_status, 0) < 0 && errno == EINTR) {}
        copy_ok = WIFEXITED(copy_status) && WEXITSTATUS(copy_status) == 0;
    }
    tui_invalidate(); /* the child drew into its own copy of the screen state */
    if (!copy_ok) {
        if (copier < 0)
            snprintf(err_buf, err_sz, "Could not start the copy process: %s", strerror(errno));
        else if (WIFSIGNALED(copy_status))
            snprintf(err_buf, err_sz, "Copy process crashed (signal %d). See %s", WTERMSIG(copy_status), LOG_PATH);
        else
            snprintf(err_buf, err_sz, "Copying system files failed. See %s", LOG_PATH);
        install_log(err_buf);
        install_unmount_target();
        return -1;
    }

    /* Create virtual & runtime mount points */
    ensure_dir(TARGET_MOUNT "/home", 0755);
    ensure_dir(TARGET_MOUNT "/proc", 0755);
    ensure_dir(TARGET_MOUNT "/sys", 0755);
    ensure_dir(TARGET_MOUNT "/dev", 0755);
    ensure_dir(TARGET_MOUNT "/run", 0755);
    ensure_dir(TARGET_MOUNT "/tmp", 0755);
    ensure_dir(TARGET_MOUNT "/mnt", 0755);
    chmod(TARGET_MOUNT "/tmp", 01777);
    chmod(TARGET_MOUNT "/run", 01777);
    ensure_dir(TARGET_MOUNT "/var/cache/axon/pkg", 0755);
    ensure_dir(TARGET_MOUNT "/var/cache/pacman/pkg", 0755);
    ensure_dir(TARGET_MOUNT "/var/lib/axon/sync", 0755);
    ensure_dir(TARGET_MOUNT "/var/log", 0755);
    ensure_dir(TARGET_MOUNT "/var/tmp", 0755);
    chmod(TARGET_MOUNT "/var/tmp", 01777);

    /* Bootloader & Kernel: everything is staged on the root's /boot first,
     * then the (unmounted) ESP is written from userspace with mtools. The
     * kernel's FAT32 driver is not trusted with this: its writes do not yet
     * update directory entries outside the volume root. */
    if (cb) cb(80, "Installing kernel and Limine bootloader...", NULL, ctx);
    ensure_dir(TARGET_MOUNT "/boot/limine", 0755);
    ensure_dir(TARGET_MOUNT "/boot/efi", 0755);

    /* The live root normally has no /boot/kernel (it boots from the ISO or
     * USB stick), so fall back to the kernel's copy of its own boot file. */
    const char *kernel_src = access("/boot/kernel", F_OK) == 0 ? "/boot/kernel" : "/proc/boot_kernel";
    if (copy_file(kernel_src, TARGET_MOUNT "/boot/kernel", 0644) != 0) {
        snprintf(err_buf, err_sz, "Failed to copy the kernel (%s) to the target", kernel_src);
        install_unmount_target();
        return -1;
    }
    if (access("/usr/share/limine/BOOTX64.EFI", R_OK) != 0) {
        snprintf(err_buf, err_sz, "/usr/share/limine/BOOTX64.EFI is missing (is limine installed?)");
        install_unmount_target();
        return -1;
    }
    bool have_bios_stage = access("/usr/share/limine/limine-bios.sys", R_OK) == 0 &&
        copy_file("/usr/share/limine/limine-bios.sys", TARGET_MOUNT "/boot/limine/limine-bios.sys", 0644) == 0;
    bool have_wallpaper = access("/usr/share/limine/boo.png", R_OK) == 0 &&
        copy_file("/usr/share/limine/boo.png", TARGET_MOUNT "/boot/limine/boo.png", 0644) == 0;

    FILE *lconf = fopen(TARGET_MOUNT "/boot/limine/limine.conf", "w");
    if (!lconf) {
        snprintf(err_buf, err_sz, "Failed to write limine.conf");
        install_unmount_target();
        return -1;
    }
    fprintf(lconf,
            "timeout: 5\n"
            "interface_resolution: 1280x800\n"
            "backdrop: 1a2b3c\n"
            "term_background: 802a3b4c\n"
            "term_foreground: ffffff\n");
    /* boot() is the partition Limine was loaded from: the ESP. */
    if (have_wallpaper) {
        fprintf(lconf,
                "wallpaper: boot():/boot/limine/boo.png\n"
                "term_wallpaper: boot():/boot/limine/boo.png\n");
    }
    const bool has_sddm = (cfg->desktop != DE_CONSOLE) &&
                          (access(TARGET_MOUNT "/usr/bin/sddm", X_OK) == 0);

    fprintf(lconf,
            "\n/AvoryOS (OpenRC)\n"
            "    protocol: limine\n"
            "    path: boot():/boot/kernel\n"
            "    cmdline: init=/sbin/init splash quiet\n"
            "    resolution: 1280x800\n\n");
    if (access(TARGET_MOUNT "/usr/lib/systemd/systemd", X_OK) == 0) {
        fprintf(lconf,
                "/AvoryOS (systemd)\n"
                "    protocol: limine\n"
                "    path: boot():/boot/kernel\n"
                "    cmdline: init=/usr/lib/systemd/systemd systemd.unit=%s systemd.log_target=console plymouth.enable=0\n"
                "    resolution: 1280x800\n\n",
                has_sddm ? "graphical.target" : "multi-user.target");
    }
    fprintf(lconf,
            "/AvoryOS (Rescue Shell)\n"
            "    protocol: limine\n"
            "    path: boot():/boot/kernel\n"
            "    cmdline: init=/bin/bash screenlog\n"
            "    resolution: 1280x800\n");

    /* Detect other OS UEFI bootloaders on an existing ESP and add chainload entries */
    if (!wipe_disk) {
        static const struct { const char *label; const char *mpath; const char *lpath; } other_os[] = {
            {"Windows Boot Manager", "::/EFI/Microsoft/Boot/bootmgfw.efi", "/EFI/Microsoft/Boot/bootmgfw.efi"},
            {"Ubuntu",               "::/EFI/ubuntu/grubx64.efi",         "/EFI/ubuntu/grubx64.efi"},
            {"Debian",               "::/EFI/debian/grubx64.efi",         "/EFI/debian/grubx64.efi"},
            {"Fedora",               "::/EFI/fedora/grubx64.efi",         "/EFI/fedora/grubx64.efi"},
            {"Arch Linux",           "::/EFI/arch/grubx64.efi",           "/EFI/arch/grubx64.efi"},
            {"Linux (systemd-boot)", "::/EFI/systemd/systemd-bootx64.efi","/EFI/systemd/systemd-bootx64.efi"},
        };
        char mdir_out[512];
        for (size_t i = 0; i < sizeof(other_os) / sizeof(other_os[0]); i++) {
            const char *mdir_argv[] = {"env", "MTOOLS_SKIP_CHECK=1", "mdir", "-b", "-i", esp_dev,
                                       other_os[i].mpath, NULL};
            if (run_cmd_capture(mdir_argv, mdir_out, sizeof(mdir_out)) == 0 && mdir_out[0]) {
                fprintf(lconf,
                        "\n/%s\n"
                        "    protocol: efi\n"
                        "    path: boot():%s\n",
                        other_os[i].label, other_os[i].lpath);
            }
        }
    }
    fclose(lconf);

    /* Limine's UEFI loader reads limine.conf next to itself first; the BIOS
     * stage searches /boot/limine and the volume root. Install to both
     * EFI/avoryos and EFI/BOOT so firmware with or without NVRAM entries boots. */
    struct { const char *src; const char *dst; bool want; } esp_files[] = {
        {"/usr/share/limine/BOOTX64.EFI", "::/EFI/BOOT/BOOTX64.EFI", true},
        {"/usr/share/limine/BOOTX64.EFI", "::/EFI/avoryos/BOOTX64.EFI", true},
        {TARGET_MOUNT "/boot/kernel", "::/boot/kernel", true},
        {TARGET_MOUNT "/boot/limine/limine.conf", "::/EFI/BOOT/limine.conf", true},
        {TARGET_MOUNT "/boot/limine/limine.conf", "::/EFI/avoryos/limine.conf", true},
        {TARGET_MOUNT "/boot/limine/limine.conf", "::/boot/limine/limine.conf", true},
        {TARGET_MOUNT "/boot/limine/limine.conf", "::/limine.conf", true},
        {TARGET_MOUNT "/boot/limine/limine-bios.sys", "::/boot/limine/limine-bios.sys", have_bios_stage},
        {TARGET_MOUNT "/boot/limine/boo.png", "::/boot/limine/boo.png", have_wallpaper},
    };
    bool esp_ok = true;
    const char *mmd_dirs[] = {"::/EFI", "::/EFI/BOOT", "::/EFI/avoryos", "::/boot", "::/boot/limine", NULL};
    for (int d_i = 0; mmd_dirs[d_i]; d_i++) {
        const char *mmd_argv[] = {"env", "MTOOLS_SKIP_CHECK=1", "mmd", "-D", "s", "-i", esp_dev,
                                  mmd_dirs[d_i], NULL};
        if (run_cmd_logged(mmd_argv, NULL, cb, 82, "Creating ESP directories...", ctx) != 0) {
            /* mmd -D s can still fail if the directory already exists with special
             * attributes or if mtools rejects the device geometry; verify with mdir. */
            char mdir_chk[256];
            const char *mdir_argv[] = {"env", "MTOOLS_SKIP_CHECK=1", "mdir", "-b", "-i", esp_dev,
                                       mmd_dirs[d_i], NULL};
            if (run_cmd_capture(mdir_argv, mdir_chk, sizeof(mdir_chk)) != 0) {
                esp_ok = false;
                break;
            }
        }
    }
    if (esp_ok) {
        for (size_t i = 0; i < sizeof(esp_files) / sizeof(esp_files[0]); i++) {
            if (!esp_files[i].want) continue;
            const char *mcopy_argv[] = {"env", "MTOOLS_SKIP_CHECK=1", "mcopy", "-o", "-i", esp_dev,
                                        esp_files[i].src, esp_files[i].dst, NULL};
            if (run_cmd_logged(mcopy_argv, NULL, cb, 83, "Writing ESP...", ctx) != 0) {
                esp_ok = false;
                break;
            }
        }
    }
    if (!esp_ok) {
        install_log("mtools ESP write failed; falling back to mounting ESP...");
        ensure_dir(TARGET_MOUNT "/boot/efi", 0755);
        const char *mnt_esp[] = {"mount", "-t", "vfat", esp_dev, TARGET_MOUNT "/boot/efi", NULL};
        if (mount(esp_dev, TARGET_MOUNT "/boot/efi", "vfat", 0, NULL) != 0 &&
            run_cmd_logged(mnt_esp, NULL, cb, 83, "Mounting ESP fallback...", ctx) != 0) {
            snprintf(err_buf, err_sz, "Failed to write ESP (%s)", esp_dev);
            install_unmount_target();
            return -1;
        }
        bool vfat_ok = true;
        for (size_t i = 0; i < sizeof(esp_files) / sizeof(esp_files[0]); i++) {
            if (!esp_files[i].want) continue;
            char dst_path[512];
            snprintf(dst_path, sizeof(dst_path), TARGET_MOUNT "/boot/efi%s", esp_files[i].dst + 2);
            char *slash = strrchr(dst_path, '/');
            if (slash) {
                *slash = '\0';
                ensure_dir(dst_path, 0755);
                *slash = '/';
            }
            if (copy_file(esp_files[i].src, dst_path, 0644) != 0) {
                snprintf(err_buf, err_sz, "Failed to write %s to the ESP", esp_files[i].dst);
                vfat_ok = false;
                break;
            }
        }
        sync();
        if (umount(TARGET_MOUNT "/boot/efi") != 0) {
            const char *um_esp[] = {"umount", TARGET_MOUNT "/boot/efi", NULL};
            run_cmd_logged(um_esp, NULL, cb, 83, "Unmounting ESP...", ctx);
        }
        if (!vfat_ok) {
            install_unmount_target();
            return -1;
        }
    }

    if (access("/sys/firmware/efi", F_OK) == 0 &&
        (access("/usr/bin/efibootmgr", X_OK) == 0 || access("/usr/sbin/efibootmgr", X_OK) == 0)) {
        char pnum_s[16] = "1";
        for (int i = 0; i < part_count; i++) {
            if (!parts[i].is_free && parts[i].role == PART_ESP && parts[i].part_num > 0) {
                snprintf(pnum_s, sizeof(pnum_s), "%d", parts[i].part_num);
                break;
            }
        }
        const char *efi_argv[] = {"efibootmgr", "-c", "-d", disk->dev_path, "-p", pnum_s,
                                  "-L", "AvoryOS", "-l", "\\EFI\\avoryos\\BOOTX64.EFI", NULL};
        run_cmd_logged(efi_argv, NULL, cb, 84, "Registering UEFI boot entry...", ctx);
    }

    if (wipe_disk || access("/sys/firmware/efi", F_OK) != 0) {
        const char *limine_argv[] = {"limine", "bios-install", disk->dev_path, NULL};
        run_cmd_logged(limine_argv, NULL, cb, 84, "Installing Limine BIOS stage...", ctx);
    }

    /* fstab with UUIDs */
    if (cb) cb(88, "Generating /etc/fstab, hostname, and keymap...", NULL, ctx);
    char root_spec[96], esp_spec[96], uuid[64];
    if (disks_get_uuid(root_dev, uuid, sizeof(uuid))) {
        snprintf(root_spec, sizeof(root_spec), "UUID=%s", uuid);
    } else {
        snprintf(root_spec, sizeof(root_spec), "%s", root_dev);
    }
    if (disks_get_uuid(esp_dev, uuid, sizeof(uuid))) {
        snprintf(esp_spec, sizeof(esp_spec), "UUID=%s", uuid);
    } else {
        snprintf(esp_spec, sizeof(esp_spec), "%s", esp_dev);
    }

    FILE *fstab = fopen(TARGET_MOUNT "/etc/fstab", "w");
    if (fstab) {
        fprintf(fstab,
                "# /etc/fstab: static file system information\n"
                "%-42s %-10s ext4   noatime,rw   0 1\n",
                root_spec, "/");

        for (int k = 0; k < n_extra; k++) {
            const part_entry_t *p = &parts[extra_idx[k]];
            char spec[96];
            if (disks_get_uuid(p->dev_path, uuid, sizeof(uuid))) {
                snprintf(spec, sizeof(spec), "UUID=%s", uuid);
            } else {
                snprintf(spec, sizeof(spec), "%s", p->dev_path);
            }
            fprintf(fstab, "%-42s %-10s ext4   noatime,rw   0 2\n", spec, p->mountpoint);
        }

        for (int i = 0; i < part_count; i++) {
            if (parts[i].is_free || parts[i].role == PART_NONE) continue;
            if (parts[i].fs == FS_SWAP) {
                char spec[96];
                if (disks_get_uuid(parts[i].dev_path, uuid, sizeof(uuid))) {
                    snprintf(spec, sizeof(spec), "UUID=%s", uuid);
                } else {
                    snprintf(spec, sizeof(spec), "%s", parts[i].dev_path);
                }
                fprintf(fstab, "%-42s %-10s swap   sw           0 0\n", spec, "none");
            }
        }

        fprintf(fstab,
                "%-42s %-10s vfat   defaults     0 0\n"
                "%-42s %-10s proc   defaults     0 0\n"
                "%-42s %-10s sysfs  defaults     0 0\n",
                esp_spec, "/boot/efi",
                "proc", "/proc",
                "sysfs", "/sys");
        fclose(fstab);
    }

    /* Hostname & hosts */
    FILE *hn = fopen(TARGET_MOUNT "/etc/hostname", "w");
    if (hn) {
        fprintf(hn, "%s\n", cfg->hostname);
        fclose(hn);
    }
    FILE *hosts = fopen(TARGET_MOUNT "/etc/hosts", "w");
    if (hosts) {
        fprintf(hosts,
                "127.0.0.1   localhost %s\n"
                "::1         localhost ip6-localhost ip6-loopback\n",
                cfg->hostname);
        fclose(hosts);
    }

    /* Keymap (TTY vconsole + Xorg XKB) */
    const char *xkb_layout = (strcmp(cfg->keymap, "uk") == 0) ? "gb" : cfg->keymap;
    FILE *vcon = fopen(TARGET_MOUNT "/etc/vconsole.conf", "w");
    if (vcon) {
        fprintf(vcon, "KEYMAP=%s\n", cfg->keymap);
        fclose(vcon);
    }
    ensure_dir(TARGET_MOUNT "/etc/X11/xorg.conf.d", 0755);
    FILE *xkb = fopen(TARGET_MOUNT "/etc/X11/xorg.conf.d/90-keyboard.conf", "w");
    if (xkb) {
        fprintf(xkb,
                "Section \"InputClass\"\n"
                "    Identifier \"system-keyboard\"\n"
                "    MatchIsKeyboard \"on\"\n"
                "    Option \"XkbLayout\" \"%s\"\n"
                "EndSection\n",
                xkb_layout);
        fclose(xkb);
    }
    if (access(TARGET_MOUNT "/usr/share/sddm/scripts/Xsetup", F_OK) == 0) {
        FILE *xs = fopen(TARGET_MOUNT "/usr/share/sddm/scripts/Xsetup", "a");
        if (xs) {
            fprintf(xs, "\ncommand -v setxkbmap >/dev/null 2>&1 && setxkbmap %s 2>/dev/null || true\n", xkb_layout);
            fclose(xs);
        }
    }

    /* Network DHCP preference */
    if (cfg->enable_dhcp) {
        unlink(TARGET_MOUNT "/etc/avory-no-dhcp");
        ensure_dir(TARGET_MOUNT "/etc/systemd/network", 0755);
        FILE *net = fopen(TARGET_MOUNT "/etc/systemd/network/20-wired.network", "w");
        if (net) {
            fputs("[Match]\nName=en* eth*\n\n[Network]\nDHCP=yes\n", net);
            fclose(net);
        }
    } else {
        FILE *nodhcp = fopen(TARGET_MOUNT "/etc/avory-no-dhcp", "w");
        if (nodhcp) fclose(nodhcp);
    }

    /* Passwords & user account */
    if (cb) cb(92, "Configuring root and user accounts...", NULL, ctx);
    if (cfg->root_password[0]) {
        set_account_password("root", cfg->root_password, cb, 93, "Setting root password...", ctx);
    }

    if (cfg->username[0]) {
        /* Remove the pre-seeded live user ("avory") when installing under a
         * different username so the chosen user gets UID 1000 and is the
         * primary account in SDDM. */
        if (strcmp(cfg->username, "avory") != 0) {
            const char *udel[] = {
                "chroot", TARGET_MOUNT, "/bin/sh", "-c",
                "userdel -r avory 2>/dev/null || true; rm -rf /home/avory",
                NULL
            };
            run_cmd_logged(udel, NULL, cb, 94, "Removing live default user...", ctx);
        }

        const char *comment = cfg->fullname[0] ? cfg->fullname : cfg->username;
        /* Use useradd -M (home dir is created and populated from /etc/skel
         * below) to avoid useradd's internal skel copy hitting EBADF; fall back
         * to direct /etc/passwd, /etc/group, /etc/shadow creation if useradd
         * fails in the live chroot. */
        const char *uadd[] = {
            "chroot", TARGET_MOUNT, "/bin/sh", "-c",
            "if id -u \"$2\" >/dev/null 2>&1; then exit 0; fi; "
            "rm -f /etc/.pwd.lock /etc/passwd.lock /etc/shadow.lock "
            "/etc/group.lock /etc/gshadow.lock /etc/subuid.lock /etc/subgid.lock; "
            "grps=''; "
            "for g in wheel audio video input; do "
            "  grep -q \"^${g}:\" /etc/group 2>/dev/null && grps=\"${grps:+$grps,}$g\"; "
            "done; "
            "if [ -n \"$grps\" ]; then "
            "  /usr/bin/useradd -M -s /bin/bash -c \"$1\" -G \"$grps\" \"$2\" || true; "
            "else "
            "  /usr/bin/useradd -M -s /bin/bash -c \"$1\" \"$2\" || true; "
            "fi; "
            "if ! id -u \"$2\" >/dev/null 2>&1; then "
            "  rm -f /etc/.pwd.lock /etc/passwd.lock /etc/shadow.lock "
            "  /etc/group.lock /etc/gshadow.lock /etc/subuid.lock /etc/subgid.lock; "
            "  uid=1000; "
            "  while awk -F: -v u=\"$uid\" '$3==u{found=1} END{exit !found}' /etc/passwd 2>/dev/null; do "
            "    uid=$((uid + 1)); "
            "  done; "
            "  gid=\"$uid\"; "
            "  if ! grep -q \"^$2:\" /etc/group 2>/dev/null; then "
            "    printf '%s:x:%s:\\n' \"$2\" \"$gid\" >> /etc/group; "
            "  else "
            "    gid=$(awk -F: -v g=\"$2\" '$1==g{print $3; exit}' /etc/group); "
            "  fi; "
            "  printf '%s:x:%s:%s:%s:/home/%s:/bin/bash\\n' \"$2\" \"$uid\" \"$gid\" \"$1\" \"$2\" >> /etc/passwd; "
            "  if [ -f /etc/shadow ] && ! grep -q \"^$2:\" /etc/shadow 2>/dev/null; then "
            "    printf '%s:!:19000:0:99999:7:::\\n' \"$2\" >> /etc/shadow; "
            "  fi; "
            "  for g in wheel audio video input; do "
            "    if grep -q \"^${g}:\" /etc/group 2>/dev/null; then "
            "      awk -F: -v OFS=: -v g=\"$g\" -v u=\"$2\" "
            "        '$1==g{if($4==\"\")$4=u; else if((\",\"$4\",\")!~(\",\"u\",\"))$4=$4\",\"u} {print}' "
            "        /etc/group > /etc/group.tmp && mv /etc/group.tmp /etc/group; "
            "    fi; "
            "  done; "
            "fi; "
            "id -u \"$2\" >/dev/null 2>&1",
            "sh", comment, cfg->username,
            NULL
        };
        if (run_cmd_logged(uadd, NULL, cb, 95, "Creating user account...", ctx) != 0) {
            snprintf(err_buf, err_sz, "Failed to create user account '%s'. See %s", cfg->username, LOG_PATH);
            install_unmount_target();
            return -1;
        }

        /* Create/populate the user's home directory from OUTSIDE chroot so
         * submounts under TARGET_MOUNT (such as a separate /home partition)
         * receive the files directly. */
        char uid_s[32] = "1000", gid_s[32] = "1000";
        const char *id_u[] = {"chroot", TARGET_MOUNT, "id", "-u", cfg->username, NULL};
        const char *id_g[] = {"chroot", TARGET_MOUNT, "id", "-g", cfg->username, NULL};
        if (run_cmd_capture(id_u, uid_s, sizeof(uid_s)) == 0) {
            uid_s[strcspn(uid_s, "\r\n ")] = '\0';
        }
        if (run_cmd_capture(id_g, gid_s, sizeof(gid_s)) == 0) {
            gid_s[strcspn(gid_s, "\r\n ")] = '\0';
        }
        char home_cmd[768];
        snprintf(home_cmd, sizeof(home_cmd),
                 "h='" TARGET_MOUNT "/home/%s'; "
                 "mkdir -p \"$h\" && cp -a '" TARGET_MOUNT "/etc/skel/.' \"$h\"/ 2>/dev/null || true; "
                 "chown -R %s:%s \"$h\" 2>/dev/null || true; "
                 "chmod 0700 \"$h\" 2>/dev/null || true",
                 cfg->username, uid_s, gid_s);
        const char *home_argv[] = {"/bin/sh", "-c", home_cmd, NULL};
        run_cmd_logged(home_argv, NULL, cb, 95, "Preparing user home directory...", ctx);

        if (cfg->user_password[0]) {
            char pw_prep[256];
            snprintf(pw_prep, sizeof(pw_prep),
                     "sed -i -E 's/^(%s):[^:]*:/\\1:x:/' /etc/passwd", cfg->username);
            const char *prep_argv[] = {"chroot", TARGET_MOUNT, "/bin/sh", "-c", pw_prep, NULL};
            run_cmd_logged(prep_argv, NULL, cb, 96, "Preparing user password...", ctx);

            set_account_password(cfg->username, cfg->user_password, cb, 96, "Setting user password...", ctx);
        } else {
            char pw_cmd[256];
            snprintf(pw_cmd, sizeof(pw_cmd),
                     "sed -i -E 's/^(%s):[^:]*:/\\1::/' /etc/passwd; "
                     "[ -f /etc/shadow ] && sed -i -E 's/^(%s):[^:]*:/\\1::/' /etc/shadow || true",
                     cfg->username, cfg->username);
            const char *pw_empty[] = {"chroot", TARGET_MOUNT, "/bin/sh", "-c", pw_cmd, NULL};
            run_cmd_logged(pw_empty, NULL, cb, 96, "Configuring passwordless user...", ctx);
        }
    }

    /* Desktop session & display manager */
    const char *xinit_cmd = NULL;
    if (cfg->desktop == DE_PLASMA) xinit_cmd = "exec startplasma-x11";
    else if (cfg->desktop == DE_XFCE4) xinit_cmd = "exec startxfce4";
    else if (cfg->desktop == DE_ICEWM) xinit_cmd = "exec icewm-session";

    if (xinit_cmd) {
        write_xinitrc(TARGET_MOUNT "/root/.xinitrc", xinit_cmd, xkb_layout);
        if (cfg->username[0]) {
            char u_xinit[256];
            snprintf(u_xinit, sizeof(u_xinit), TARGET_MOUNT "/home/%s/.xinitrc", cfg->username);
            write_xinitrc(u_xinit, xinit_cmd, xkb_layout);
            char uid_s[32] = "1000", gid_s[32] = "1000";
            const char *id_u[] = {"chroot", TARGET_MOUNT, "id", "-u", cfg->username, NULL};
            const char *id_g[] = {"chroot", TARGET_MOUNT, "id", "-g", cfg->username, NULL};
            if (run_cmd_capture(id_u, uid_s, sizeof(uid_s)) == 0) uid_s[strcspn(uid_s, "\r\n ")] = '\0';
            if (run_cmd_capture(id_g, gid_s, sizeof(gid_s)) == 0) gid_s[strcspn(gid_s, "\r\n ")] = '\0';
            chown(u_xinit, (uid_t)strtoul(uid_s, NULL, 10), (gid_t)strtoul(gid_s, NULL, 10));
        }
    }

    if (has_sddm) {
        FILE *inittab = fopen(TARGET_MOUNT "/etc/inittab", "w");
        if (inittab) {
            fputs("# AvoryOS installed desktop: run services once, then launch SDDM on tty1.\n"
                  "# Unset BusyBox init's HOME=/ and XDG_* so sddm-helper resolves each user's home.\n"
                  "::sysinit:/etc/init.d/rcS\n"
                  "tty1::respawn:/usr/bin/env -u HOME -u XDG_DATA_HOME -u XDG_CONFIG_HOME -u XDG_CACHE_HOME -u XDG_STATE_HOME /usr/bin/sddm\n"
                  "\n"
                  "::ctrlaltdel:/usr/bin/busybox reboot\n"
                  "::shutdown:/usr/bin/busybox umount -a -r\n",
                  inittab);
            fclose(inittab);
        }

        const char *sddm_chown[] = {
            "chroot", TARGET_MOUNT, "/bin/sh", "-c",
            "chown -R sddm:sddm /var/lib/sddm 2>/dev/null || true",
            NULL
        };
        run_cmd_logged(sddm_chown, NULL, cb, 97, "Configuring display manager...", ctx);

        ensure_dir(TARGET_MOUNT "/etc/sddm.conf.d", 0755);
        ensure_dir(TARGET_MOUNT "/etc/sddm", 0755);
        FILE *dsetup = fopen(TARGET_MOUNT "/etc/sddm/display-setup.sh", "w");
        if (dsetup) {
            fprintf(dsetup,
                    "#!/bin/sh\n"
                    "[ -x /usr/bin/avory-prime-display ] && /usr/bin/avory-prime-display || true\n"
                    "command -v setxkbmap >/dev/null 2>&1 && setxkbmap %s 2>/dev/null || true\n",
                    xkb_layout);
            fclose(dsetup);
            chmod(TARGET_MOUNT "/etc/sddm/display-setup.sh", 0755);
        }
        FILE *vtconf = fopen(TARGET_MOUNT "/etc/sddm.conf.d/99-avory.conf", "w");
        if (vtconf) {
            fputs("[X11]\nMinimumVT=1\nSessionLogFile=/tmp/xorg-session.log\n"
                  "DisplayCommand=/etc/sddm/display-setup.sh\n", vtconf);
            fclose(vtconf);
        }
        if (access(TARGET_MOUNT "/usr/share/sddm/themes/breeze", F_OK) != 0) {
            unlink(TARGET_MOUNT "/etc/sddm.conf.d/10-theme.conf");
        }

        if (access(TARGET_MOUNT "/usr/lib/systemd/system/sddm.service", F_OK) == 0) {
            ensure_dir(TARGET_MOUNT "/etc/systemd/system", 0755);
            unlink(TARGET_MOUNT "/etc/systemd/system/multi-user.target.wants/avory-login.service");
            unlink(TARGET_MOUNT "/etc/systemd/system/display-manager.service");
            if (symlink("/usr/lib/systemd/system/sddm.service",
                        TARGET_MOUNT "/etc/systemd/system/display-manager.service") != 0) {
                install_log("Warning: failed to enable systemd display-manager.service");
            }
        }
    } else if (cfg->desktop == DE_CONSOLE &&
               (access(TARGET_MOUNT "/usr/local/bin/startx-xfce4", F_OK) == 0 ||
                access(TARGET_MOUNT "/usr/local/bin/startx-icewm", F_OK) == 0)) {
        FILE *inittab = fopen(TARGET_MOUNT "/etc/inittab", "w");
        if (inittab) {
            const char *tty1_cmd = access(TARGET_MOUNT "/bin/avory-login", X_OK) == 0
                ? "/bin/avory-login" : "/bin/bash -l";
            fprintf(inittab,
                    "::sysinit:/bin/mount -t proc proc /proc\n"
                    "::sysinit:/bin/mount -t sysfs sysfs /sys\n"
                    "::sysinit:/bin/mount -t tmpfs -o rw,nosuid,nodev,mode=1777 tmpfs /tmp\n"
                    "::sysinit:/bin/mount -t tmpfs -o rw,nosuid,nodev,mode=0755 tmpfs /run\n"
                    "::sysinit:/bin/mkdir -p /run/dbus /run/user/0 /tmp/.X11-unix\n");
            if (access(TARGET_MOUNT "/usr/local/bin/avory-net-init", X_OK) == 0) {
                fputs("::sysinit:/usr/local/bin/avory-net-init\n", inittab);
            }
            if (access(TARGET_MOUNT "/usr/local/bin/avory-xorg-init", X_OK) == 0) {
                fputs("::sysinit:/usr/local/bin/avory-xorg-init\n", inittab);
            }
            fprintf(inittab,
                    "tty1::respawn:%s\n"
                    "::shutdown:/bin/umount -a -r\n"
                    "::ctrlaltdel:/sbin/reboot\n",
                    tty1_cmd);
            fclose(inittab);
        }
    }

    if (cb) cb(98, "Syncing disks...", NULL, ctx);
    sync();
    if (cb) cb(100, "Installation complete.", NULL, ctx);
    return 0;
}
