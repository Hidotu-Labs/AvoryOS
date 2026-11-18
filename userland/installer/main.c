#include "disks.h"
#include "install.h"
#include "installer.h"
#include "tui.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static disk_t g_disks[MAX_DISKS];
static int g_disk_count = 0;

static const char *k_keymaps[][2] = {
    {"us", "US English"},
    {"uk", "UK English"},
    {"de", "German"},
    {"fr", "French"},
    {"es", "Spanish"},
    {"it", "Italian"},
    {"se", "Swedish"},
    {"no", "Norwegian"},
    {"tr", "Turkish (Q)"},
};

static const char *desktop_name(desktop_choice_t d) {
    switch (d) {
        case DE_PLASMA:  return "KDE Plasma 6";
        case DE_XFCE4:   return "XFCE 4";
        case DE_ICEWM:   return "IceWM";
        case DE_CONSOLE: return "Console only";
        default:         return "Unknown";
    }
}

static void pick_keymap(installer_cfg_t *cfg) {
    int count = (int)(sizeof(k_keymaps) / sizeof(k_keymaps[0]));
    int sel = 0;
    for (int i = 0; i < count; i++) {
        if (strcmp(cfg->keymap, k_keymaps[i][0]) == 0) {
            sel = i;
            break;
        }
    }

    int w = 56, h = count + 6;
    while (1) {
        int x = (g_term_cols - w) / 2;
        int y = (g_term_rows - h) / 2;
        tui_draw_chrome("Keyboard Layout", "Up/Down/j/k: Move   Enter: Select   Esc: Back");
        tui_draw_box(y, x, h, w, "Select Keymap", C_BORDER, C_PANEL);

        for (int i = 0; i < count; i++) {
            bool active = (i == sel);
            uint8_t bg = active ? C_SEL_BG : C_PANEL;
            uint8_t fg = active ? C_SEL_FG : C_TEXT;
            tui_fill_rect(y + 2 + i, x + 2, 1, w - 4, fg, bg, ' ');
            tui_printf(y + 2 + i, x + 4, fg, bg, active ? 1 : 0, "%-6s  %s", k_keymaps[i][0], k_keymaps[i][1]);
        }
        tui_flush();

        int k = tui_read_key();
        if (k == KEY_ESC) return;
        if (k == KEY_UP || k == 'k') sel = (sel - 1 + count) % count;
        else if (k == KEY_DOWN || k == 'j') sel = (sel + 1) % count;
        else if (k == KEY_ENTER) {
            snprintf(cfg->keymap, sizeof(cfg->keymap), "%s", k_keymaps[sel][0]);
            return;
        }
    }
}

static void pick_network(installer_cfg_t *cfg) {
    int sel = cfg->enable_dhcp ? 0 : 1;
    int w = 60, h = 10;
    while (1) {
        int x = (g_term_cols - w) / 2;
        int y = (g_term_rows - h) / 2;
        tui_draw_chrome("Network", "Up/Down: Move   Enter: Select   Esc: Back");
        tui_draw_box(y, x, h, w, "Network Configuration", C_BORDER, C_PANEL);

        const char *opts[2] = {
            "Automatic (DHCP) - configure wired interfaces on boot",
            "Manual           - leave network unconfigured",
        };
        for (int i = 0; i < 2; i++) {
            bool active = (i == sel);
            uint8_t bg = active ? C_SEL_BG : C_PANEL;
            uint8_t fg = active ? C_SEL_FG : C_TEXT;
            tui_fill_rect(y + 3 + i * 2, x + 2, 1, w - 4, fg, bg, ' ');
            tui_put_str_n(y + 3 + i * 2, x + 4, w - 8, fg, bg, active ? 1 : 0, opts[i]);
        }
        tui_flush();

        int k = tui_read_key();
        if (k == KEY_ESC) return;
        if (k == KEY_UP || k == KEY_DOWN || k == 'j' || k == 'k') sel = !sel;
        else if (k == KEY_ENTER) {
            cfg->enable_dhcp = (sel == 0);
            return;
        }
    }
}

static void pick_disk(installer_cfg_t *cfg) {
    g_disk_count = disks_scan(g_disks, MAX_DISKS);
    if (g_disk_count == 0) {
        tui_modal_message("No Disks Found", "No block devices were detected in /sys/block.", NULL, true);
        return;
    }

    int sel = (cfg->disk_idx >= 0 && cfg->disk_idx < g_disk_count) ? cfg->disk_idx : 0;
    int w = 74, h = g_disk_count + 8;
    if (h > g_term_rows - 4) h = g_term_rows - 4;

    while (1) {
        int x = (g_term_cols - w) / 2;
        int y = (g_term_rows - h) / 2;
        tui_draw_chrome("Disk Configuration", "Up/Down/j/k: Move   Enter: Select Disk   r: Rescan   Esc: Back");
        tui_draw_box(y, x, h, w, "Select Target Block Device (GPT + ESP + ext4)", C_BORDER, C_PANEL);

        tui_put_str(y + 2, x + 3, C_MUTED, C_PANEL, 1, "Device          Size        Model                     Status");
        for (int i = 0; i < g_disk_count && i < h - 6; i++) {
            bool active = (i == sel);
            uint8_t bg = active ? C_SEL_BG : C_PANEL;
            uint8_t fg = g_disks[i].is_live_media ? C_ERR : (active ? C_SEL_FG : C_TEXT);
            char sz[24];
            disks_format_size(g_disks[i].size_bytes, sz, sizeof(sz));

            tui_fill_rect(y + 4 + i, x + 2, 1, w - 4, fg, bg, ' ');
            tui_printf(y + 4 + i, x + 3, fg, bg, active ? 1 : 0,
                       "%-14s  %-10s  %-24.24s  %s",
                       g_disks[i].dev_path, sz, g_disks[i].model,
                       g_disks[i].is_live_media ? "[LIVE MEDIA]" : "Available");
        }
        tui_flush();

        int k = tui_read_key();
        if (k == KEY_ESC) return;
        if (k == 'r') {
            g_disk_count = disks_scan(g_disks, MAX_DISKS);
            if (sel >= g_disk_count) sel = g_disk_count > 0 ? g_disk_count - 1 : 0;
        } else if (k == KEY_UP || k == 'k') {
            sel = (sel - 1 + g_disk_count) % g_disk_count;
        } else if (k == KEY_DOWN || k == 'j') {
            sel = (sel + 1) % g_disk_count;
        } else if (k == KEY_ENTER) {
            if (g_disks[sel].is_live_media) {
                tui_modal_message("Protected Disk",
                                  "Cannot install onto the active live media mounted on /.",
                                  g_disks[sel].dev_path, true);
                continue;
            }
            cfg->disk_idx = sel;
            return;
        }
    }
}

static void pick_desktop(installer_cfg_t *cfg) {
    int sel = (int)cfg->desktop;
    int count = 4;
    int w = 56, h = 11;
    while (1) {
        int x = (g_term_cols - w) / 2;
        int y = (g_term_rows - h) / 2;
        tui_draw_chrome("Desktop Environment", "Up/Down/j/k: Move   Enter: Select   Esc: Back");
        tui_draw_box(y, x, h, w, "Select Default Session", C_BORDER, C_PANEL);

        for (int i = 0; i < count; i++) {
            bool active = (i == sel);
            uint8_t bg = active ? C_SEL_BG : C_PANEL;
            uint8_t fg = active ? C_SEL_FG : C_TEXT;
            tui_fill_rect(y + 3 + i, x + 2, 1, w - 4, fg, bg, ' ');
            tui_printf(y + 3 + i, x + 4, fg, bg, active ? 1 : 0, "%s", desktop_name((desktop_choice_t)i));
        }
        tui_flush();

        int k = tui_read_key();
        if (k == KEY_ESC) return;
        if (k == KEY_UP || k == 'k') sel = (sel - 1 + count) % count;
        else if (k == KEY_DOWN || k == 'j') sel = (sel + 1) % count;
        else if (k == KEY_ENTER) {
            cfg->desktop = (desktop_choice_t)sel;
            return;
        }
    }
}

static void edit_user_account(installer_cfg_t *cfg) {
    char uname[32], fname[64], pw[64], pw2[64];
    snprintf(uname, sizeof(uname), "%s", cfg->username);
    snprintf(fname, sizeof(fname), "%s", cfg->fullname);
    snprintf(pw, sizeof(pw), "%s", cfg->user_password);
    snprintf(pw2, sizeof(pw2), "%s", cfg->user_password);

    if (!tui_prompt_text("User Account", "Username (lowercase, no spaces):", uname, sizeof(uname), false, false)) return;
    for (size_t i = 0; uname[i]; i++) {
        if (!isalnum((unsigned char)uname[i]) && uname[i] != '_' && uname[i] != '-') {
            tui_modal_message("Invalid Username", "Use only letters, digits, '_' or '-'.", NULL, true);
            return;
        }
    }
    tui_prompt_text("User Account", "Full Name (optional):", fname, sizeof(fname), false, true);
    if (!tui_prompt_text("User Password", "Password for user:", pw, sizeof(pw), true, true)) return;
    if (pw[0]) {
        if (!tui_prompt_text("User Password", "Confirm password:", pw2, sizeof(pw2), true, false)) return;
        if (strcmp(pw, pw2) != 0) {
            tui_modal_message("Password Mismatch", "The two passwords did not match.", NULL, true);
            return;
        }
    }
    snprintf(cfg->username, sizeof(cfg->username), "%s", uname);
    snprintf(cfg->fullname, sizeof(cfg->fullname), "%s", fname);
    snprintf(cfg->user_password, sizeof(cfg->user_password), "%s", pw);
}

static void edit_root_password(installer_cfg_t *cfg) {
    char pw[64] = {0}, pw2[64] = {0};
    if (!tui_prompt_text("Root Password", "Enter new root password (leave empty to skip):", pw, sizeof(pw), true, true)) return;
    if (pw[0]) {
        if (!tui_prompt_text("Root Password", "Confirm root password:", pw2, sizeof(pw2), true, false)) return;
        if (strcmp(pw, pw2) != 0) {
            tui_modal_message("Password Mismatch", "The two passwords did not match.", NULL, true);
            return;
        }
    }
    snprintf(cfg->root_password, sizeof(cfg->root_password), "%s", pw);
}

/* Live installation progress & log tail state */
#define LOG_TAIL_LINES 10
typedef struct {
    int pct;
    char stage[128];
    char lines[LOG_TAIL_LINES][128];
} prog_state_t;

static void on_install_progress(int pct, const char *stage, const char *log_line, void *ctx) {
    prog_state_t *st = (prog_state_t *)ctx;
    st->pct = pct;
    if (stage && stage[0]) {
        snprintf(st->stage, sizeof(st->stage), "%s", stage);
    }
    if (log_line && log_line[0]) {
        for (int i = 0; i < LOG_TAIL_LINES - 1; i++) {
            memcpy(st->lines[i], st->lines[i + 1], sizeof(st->lines[i]));
        }
        snprintf(st->lines[LOG_TAIL_LINES - 1], sizeof(st->lines[0]), "%s", log_line);
    }

    int w = g_term_cols - 8;
    if (w > 86) w = 86;
    int h = 20;
    int x = (g_term_cols - w) / 2;
    int y = (g_term_rows - h) / 2;

    tui_draw_chrome("Installing AvoryOS", "Please wait...  Log: " LOG_PATH);
    tui_draw_box(y, x, h, w, "Installation Progress", C_ACCENT, C_PANEL);

    tui_printf(y + 2, x + 3, C_BRIGHT, C_PANEL, 1, "%3d%%  %s", st->pct, st->stage);

    int bar_w = w - 6;
    int filled = (bar_w * st->pct) / 100;
    for (int i = 0; i < bar_w; i++) {
        tui_put_cell(y + 4, x + 3 + i, i < filled ? C_ACCENT : C_BORDER, C_PANEL, 0,
                     i < filled ? 0x2588 : 0x2591);
    }

    tui_put_str(y + 6, x + 3, C_MUTED, C_PANEL, 0, "Recent output:");
    for (int i = 0; i < LOG_TAIL_LINES; i++) {
        tui_put_str_n(y + 7 + i, x + 3, w - 6, C_TEXT, C_PANEL, 0, st->lines[i]);
    }
    tui_flush();
}

static bool run_install_flow(installer_cfg_t *cfg) {
    if (cfg->disk_idx < 0 || cfg->disk_idx >= g_disk_count) {
        tui_modal_message("Missing Disk", "Please select a target disk first.", NULL, true);
        return false;
    }

    disk_t *d = &g_disks[cfg->disk_idx];
    char sz[24], warn_line[128];
    disks_format_size(d->size_bytes, sz, sizeof(sz));
    snprintf(warn_line, sizeof(warn_line), "ALL DATA ON %s (%s, %s) WILL BE ERASED!", d->dev_path, sz, d->model);

    if (!tui_modal_confirm(cfg->dry_run ? "Confirm Dry-Run" : "Confirm Disk Wipe",
                           warn_line,
                           "Layout: 128 MiB FAT32 ESP + ext4 Root (Limine UEFI & BIOS)",
                           !cfg->dry_run)) {
        return false;
    }

    prog_state_t st;
    memset(&st, 0, sizeof(st));
    char err_buf[256] = {0};

    int rc = install_system(cfg, d, on_install_progress, &st, err_buf, sizeof(err_buf));
    if (rc != 0) {
        tui_modal_message("Installation Failed", err_buf[0] ? err_buf : "See " LOG_PATH,
                          "Target filesystems have been unmounted.", true);
        return false;
    }

    /* Post-install menu */
    int sel = 0;
    const char *btns[3] = {"Reboot Now", "Chroot Shell", "Exit Installer"};
    int w = 64, h = 13;
    while (1) {
        int x = (g_term_cols - w) / 2;
        int y = (g_term_rows - h) / 2;
        tui_draw_chrome("Complete", "Left/Right: Select   Enter: Run");
        tui_draw_box(y, x, h, w, "Installation Complete", C_OK, C_PANEL);
        tui_put_str(y + 3, x + 4, C_OK, C_PANEL, 1, "AvoryOS has been installed successfully.");
        tui_put_str(y + 5, x + 4, C_TEXT, C_PANEL, 0, "Remove live installation media before rebooting.");
        tui_put_str(y + 6, x + 4, C_MUTED, C_PANEL, 0, "Full log saved to " LOG_PATH);

        for (int i = 0; i < 3; i++) {
            tui_draw_button(y + h - 3, x + 4 + i * 19, btns[i], sel == i, false);
        }
        tui_flush();

        int k = tui_read_key();
        if (k == KEY_LEFT || k == KEY_BACKTAB) sel = (sel + 2) % 3;
        else if (k == KEY_RIGHT || k == KEY_TAB) sel = (sel + 1) % 3;
        else if (k == KEY_ENTER) {
            if (sel == 1 && !cfg->dry_run) {
                tui_restore();
                printf("\n[*] Entering chroot in %s (type 'exit' to return)...\n", TARGET_MOUNT);
                install_enter_chroot();
                tui_init();
                continue;
            }
            if (!cfg->dry_run) install_unmount_target();
            if (sel == 0) {
                tui_restore();
                sync();
                execlp("reboot", "reboot", (char *)NULL);
                exit(0);
            }
            return true;
        }
    }
}

int main(int argc, char **argv) {
    installer_cfg_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    snprintf(cfg.keymap, sizeof(cfg.keymap), "us");
    snprintf(cfg.hostname, sizeof(cfg.hostname), "avoryos");
    cfg.enable_dhcp = true;
    cfg.disk_idx = -1;
    snprintf(cfg.username, sizeof(cfg.username), "avory");
    snprintf(cfg.fullname, sizeof(cfg.fullname), "AvoryOS User");
    cfg.desktop = DE_PLASMA;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            printf("AvoryOS Installer v%s\n"
                   "Usage: %s [--dry-run] [-h|--help] [-v|--version]\n",
                   INSTALLER_VERSION, argv[0]);
            return 0;
        }
        if (strcmp(argv[i], "--version") == 0 || strcmp(argv[i], "-v") == 0) {
            printf("avory-installer %s\n", INSTALLER_VERSION);
            return 0;
        }
        if (strcmp(argv[i], "--dry-run") == 0) {
            cfg.dry_run = true;
        }
    }

    if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) {
        fprintf(stderr, "avory-installer: interactive TTY required.\n");
        return 1;
    }

    g_disk_count = disks_scan(g_disks, MAX_DISKS);
    tui_init();

    const char *items[] = {
        "Keyboard layout",
        "Network configuration",
        "Disk configuration",
        "Hostname",
        "Root password",
        "User account",
        "Desktop environment",
        "Install",
        "Abort",
    };
    const int n_items = 9;
    int sel = 0;

    while (1) {
        int w = g_term_cols - 6;
        if (w > 88) w = 88;
        int h = n_items + 8;
        int x = (g_term_cols - w) / 2;
        int y = (g_term_rows - h) / 2;

        tui_draw_chrome(cfg.dry_run ? "DRY-RUN MODE" : "Guided Setup",
                        "Up/Down/j/k: Navigate   Enter: Edit/Run   q: Quit");
        tui_draw_box(y, x, h, w, "AvoryOS Installation Menu", C_BORDER, C_PANEL);

        for (int i = 0; i < n_items; i++) {
            int ry = y + 2 + i + (i >= 7 ? 1 : 0);
            bool active = (i == sel);
            uint8_t bg = active ? C_SEL_BG : C_PANEL;
            uint8_t fg = active ? C_SEL_FG : C_TEXT;

            if (i == 7) {
                for (int c = x + 2; c < x + w - 2; c++) {
                    tui_put_cell(y + 9, c, C_BORDER, C_PANEL, 0, 0x2500);
                }
            }

            tui_fill_rect(ry, x + 2, 1, w - 4, fg, bg, ' ');

            char val[96] = {0};
            uint8_t val_fg = active ? C_SEL_FG : C_MUTED;
            if (i == 0) {
                snprintf(val, sizeof(val), "%s", cfg.keymap);
                val_fg = active ? C_SEL_FG : C_OK;
            } else if (i == 1) {
                snprintf(val, sizeof(val), "%s", cfg.enable_dhcp ? "Automatic (DHCP)" : "Manual");
                val_fg = active ? C_SEL_FG : C_OK;
            } else if (i == 2) {
                if (cfg.disk_idx >= 0 && cfg.disk_idx < g_disk_count) {
                    char sz[24];
                    disks_format_size(g_disks[cfg.disk_idx].size_bytes, sz, sizeof(sz));
                    snprintf(val, sizeof(val), "%s (%s)", g_disks[cfg.disk_idx].dev_path, sz);
                    val_fg = active ? C_SEL_FG : C_OK;
                } else {
                    snprintf(val, sizeof(val), "[not selected]");
                    val_fg = active ? C_SEL_FG : C_WARN;
                }
            } else if (i == 3) {
                snprintf(val, sizeof(val), "%s", cfg.hostname);
                val_fg = active ? C_SEL_FG : C_OK;
            } else if (i == 4) {
                snprintf(val, sizeof(val), "%s", cfg.root_password[0] ? "[password set]" : "[not set]");
                val_fg = active ? C_SEL_FG : (cfg.root_password[0] ? C_OK : C_MUTED);
            } else if (i == 5) {
                snprintf(val, sizeof(val), "%s%s", cfg.username,
                         cfg.user_password[0] ? " (password set)" : " (no password)");
                val_fg = active ? C_SEL_FG : C_OK;
            } else if (i == 6) {
                snprintf(val, sizeof(val), "%s", desktop_name(cfg.desktop));
                val_fg = active ? C_SEL_FG : C_OK;
            } else if (i == 7) {
                fg = active ? C_SEL_FG : C_ACCENT;
            } else if (i == 8) {
                fg = active ? C_SEL_FG : C_ERR;
            }

            tui_put_str(ry, x + 4, fg, bg, (active || i >= 7) ? 1 : 0, items[i]);
            if (val[0]) {
                tui_put_str_n(ry, x + 30, w - 34, val_fg, bg, 0, val);
            }
        }
        tui_flush();

        int k = tui_read_key();
        if (k == 'q' || k == KEY_ESC) break;
        if (k == KEY_UP || k == 'k') sel = (sel - 1 + n_items) % n_items;
        else if (k == KEY_DOWN || k == 'j') sel = (sel + 1) % n_items;
        else if (k == KEY_ENTER) {
            switch (sel) {
                case 0: pick_keymap(&cfg); break;
                case 1: pick_network(&cfg); break;
                case 2: pick_disk(&cfg); break;
                case 3: tui_prompt_text("Hostname", "System hostname:", cfg.hostname, sizeof(cfg.hostname), false, false); break;
                case 4: edit_root_password(&cfg); break;
                case 5: edit_user_account(&cfg); break;
                case 6: pick_desktop(&cfg); break;
                case 7:
                    if (run_install_flow(&cfg)) goto done;
                    break;
                case 8: goto done;
            }
        }
    }

done:
    tui_restore();
    return 0;
}
