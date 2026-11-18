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
    umount2(TARGET_MOUNT "/boot/efi", 2);
    umount2(TARGET_MOUNT, 2);
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

static int write_xinitrc(const char *path, const char *xinit_cmd) {
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
    fprintf(f, "%s\n", xinit_cmd);
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
    if (disk->size_bytes < need) {
        char have_s[32], need_s[32];
        disks_format_size(disk->size_bytes, have_s, sizeof(have_s));
        disks_format_size(need, need_s, sizeof(need_s));
        snprintf(err_buf, err_sz, "%s is too small (%s); this selection needs about %s",
                 disk->dev_path, have_s, need_s);
        pkgsel_free(sel);
        return -1;
    }

    char esp_dev[72] = {0};
    char root_dev[72] = {0};

    if (disks_partition_and_format(disk, esp_dev, sizeof(esp_dev), root_dev, sizeof(root_dev), cb, ctx) != 0) {
        snprintf(err_buf, err_sz, "Partitioning or formatting %s failed. See %s", disk->dev_path, LOG_PATH);
        pkgsel_free(sel);
        return -1;
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
    fclose(lconf);

    /* Limine's UEFI loader reads limine.conf next to itself first; the BIOS
     * stage searches /boot/limine and the volume root. */
    struct { const char *src; const char *dst; bool want; } esp_files[] = {
        {"/usr/share/limine/BOOTX64.EFI", "::/EFI/BOOT/BOOTX64.EFI", true},
        {TARGET_MOUNT "/boot/kernel", "::/boot/kernel", true},
        {TARGET_MOUNT "/boot/limine/limine.conf", "::/EFI/BOOT/limine.conf", true},
        {TARGET_MOUNT "/boot/limine/limine.conf", "::/boot/limine/limine.conf", true},
        {TARGET_MOUNT "/boot/limine/limine.conf", "::/limine.conf", true},
        {TARGET_MOUNT "/boot/limine/limine-bios.sys", "::/boot/limine/limine-bios.sys", have_bios_stage},
        {TARGET_MOUNT "/boot/limine/boo.png", "::/boot/limine/boo.png", have_wallpaper},
    };
    const char *mmd_argv[] = {"env", "MTOOLS_SKIP_CHECK=1", "mmd", "-i", esp_dev,
                              "::/EFI", "::/EFI/BOOT", "::/boot", "::/boot/limine", NULL};
    if (run_cmd_logged(mmd_argv, NULL, cb, 82, "Creating ESP directories...", ctx) != 0) {
        snprintf(err_buf, err_sz, "Failed to create ESP directories on %s (is mtools installed?)", esp_dev);
        install_unmount_target();
        return -1;
    }
    for (size_t i = 0; i < sizeof(esp_files) / sizeof(esp_files[0]); i++) {
        if (!esp_files[i].want) continue;
        const char *mcopy_argv[] = {"env", "MTOOLS_SKIP_CHECK=1", "mcopy", "-o", "-i", esp_dev,
                                    esp_files[i].src, esp_files[i].dst, NULL};
        if (run_cmd_logged(mcopy_argv, NULL, cb, 83, "Writing ESP...", ctx) != 0) {
            snprintf(err_buf, err_sz, "Failed to write %s to the ESP", esp_files[i].dst);
            install_unmount_target();
            return -1;
        }
    }

    const char *limine_argv[] = {"limine", "bios-install", disk->dev_path, NULL};
    run_cmd_logged(limine_argv, NULL, cb, 84, "Installing Limine BIOS stage...", ctx);

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
                "%-42s /          ext4   noatime,rw   0 1\n"
                "# The ESP is maintained with mtools; the kernel's FAT32 writes are\n"
                "# not yet safe for it, so it is not mounted at boot.\n"
                "%-42s /boot/efi  vfat   noauto       0 0\n"
                "tmpfs                                      /tmp       tmpfs  defaults     0 0\n"
                "proc                                       /proc      proc   defaults     0 0\n"
                "sysfs                                      /sys       sysfs  defaults     0 0\n",
                root_spec, esp_spec);
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

    /* Keymap */
    FILE *vcon = fopen(TARGET_MOUNT "/etc/vconsole.conf", "w");
    if (vcon) {
        fprintf(vcon, "KEYMAP=%s\n", cfg->keymap);
        fclose(vcon);
    }

    /* Network DHCP preference */
    if (cfg->enable_dhcp) {
        ensure_dir(TARGET_MOUNT "/etc/systemd/network", 0755);
        FILE *net = fopen(TARGET_MOUNT "/etc/systemd/network/20-wired.network", "w");
        if (net) {
            fputs("[Match]\nName=en* eth*\n\n[Network]\nDHCP=yes\n", net);
            fclose(net);
        }
    }

    /* Passwords & user account */
    if (cb) cb(92, "Configuring root and user accounts...", NULL, ctx);
    if (cfg->root_password[0]) {
        char pw_in[128];
        snprintf(pw_in, sizeof(pw_in), "root:%s\n", cfg->root_password);
        const char *chpw[] = {"chpasswd", "--root", TARGET_MOUNT, NULL};
        run_cmd_logged(chpw, pw_in, cb, 93, "Setting root password...", ctx);
    }

    if (cfg->username[0]) {
        const char *comment = cfg->fullname[0] ? cfg->fullname : cfg->username;
        const char *uadd[] = {
            "chroot", TARGET_MOUNT, "/usr/sbin/useradd",
            "-m", "-s", "/bin/bash",
            "-c", comment,
            "-G", "wheel,audio,video,input",
            cfg->username,
            NULL
        };
        run_cmd_logged(uadd, NULL, cb, 95, "Creating user account...", ctx);

        /* The live rootfs may already define the default user (e.g. "avory")
         * in /etc/passwd while /home is not copied, so useradd skips home
         * creation. Ensure the home directory exists and is owned by the user. */
        char home_cmd[512];
        snprintf(home_cmd, sizeof(home_cmd),
                 "h=$(getent passwd %s | cut -d: -f6); "
                 "[ -n \"$h\" ] || h=/home/%s; "
                 "if [ ! -d \"$h\" ]; then mkdir -p \"$h\" && cp -a /etc/skel/. \"$h\"/ 2>/dev/null || true; fi; "
                 "chown -R %s:%s \"$h\" 2>/dev/null || true",
                 cfg->username, cfg->username, cfg->username, cfg->username);
        const char *home_argv[] = {"chroot", TARGET_MOUNT, "/bin/sh", "-c", home_cmd, NULL};
        run_cmd_logged(home_argv, NULL, cb, 95, "Preparing user home directory...", ctx);

        if (cfg->user_password[0]) {
            char pw_in[128];
            snprintf(pw_in, sizeof(pw_in), "%s:%s\n", cfg->username, cfg->user_password);
            const char *chpw[] = {"chpasswd", "--root", TARGET_MOUNT, NULL};
            run_cmd_logged(chpw, pw_in, cb, 96, "Setting user password...", ctx);
        }
    }

    /* Desktop session & display manager */
    const char *xinit_cmd = NULL;
    if (cfg->desktop == DE_PLASMA) xinit_cmd = "exec startplasma-x11";
    else if (cfg->desktop == DE_XFCE4) xinit_cmd = "exec startxfce4";
    else if (cfg->desktop == DE_ICEWM) xinit_cmd = "exec icewm-session";

    if (xinit_cmd) {
        write_xinitrc(TARGET_MOUNT "/root/.xinitrc", xinit_cmd);
        if (cfg->username[0]) {
            char u_xinit[256];
            snprintf(u_xinit, sizeof(u_xinit), TARGET_MOUNT "/home/%s/.xinitrc", cfg->username);
            write_xinitrc(u_xinit, xinit_cmd);
        }
    }

    if (has_sddm) {
        FILE *inittab = fopen(TARGET_MOUNT "/etc/inittab", "w");
        if (inittab) {
            fputs("# AvoryOS installed desktop: run services once, then launch SDDM on tty1.\n"
                  "# Unset BusyBox init's HOME=/ so sddm-helper resolves each user's home.\n"
                  "::sysinit:/etc/init.d/rcS\n"
                  "tty1::respawn:/usr/bin/env -u HOME /usr/bin/sddm\n"
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
        FILE *vtconf = fopen(TARGET_MOUNT "/etc/sddm.conf.d/00-vt.conf", "w");
        if (vtconf) {
            fputs("[X11]\nMinimumVT=1\nSessionLogFile=/tmp/xorg-session.log\n", vtconf);
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
    }

    if (cb) cb(98, "Syncing disks...", NULL, ctx);
    sync();
    if (cb) cb(100, "Installation complete.", NULL, ctx);
    return 0;
}
