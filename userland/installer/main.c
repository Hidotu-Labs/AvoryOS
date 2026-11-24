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

static bool parse_size_mib(const char *s, uint64_t *out_mib) {
    if (!s || !s[0]) return false;
    while (*s == ' ') s++;
    if (strcasecmp(s, "rest") == 0 || strcmp(s, "0") == 0) {
        *out_mib = 0;
        return true;
    }
    char *end = NULL;
    unsigned long long val = strtoull(s, &end, 10);
    if (end == s || val == 0) return false;
    while (*end == ' ') end++;
    if (*end == 'G' || *end == 'g') {
        *out_mib = (uint64_t)val * 1024ULL;
        return true;
    }
    if (*end == 'M' || *end == 'm' || *end == '\0') {
        *out_mib = (uint64_t)val;
        return true;
    }
    return false;
}

static void edit_single_part(part_entry_t *p) {
    char sz_buf[32];
    if (p->size_mib == 0) snprintf(sz_buf, sizeof(sz_buf), "0");
    else if (p->size_mib >= 1024 && (p->size_mib % 1024) == 0) snprintf(sz_buf, sizeof(sz_buf), "%lluG", (unsigned long long)(p->size_mib / 1024));
    else snprintf(sz_buf, sizeof(sz_buf), "%lluM", (unsigned long long)p->size_mib);

    if (tui_prompt_text("Partition Size", "Size (e.g. 20G, 4096M, or 0 for remaining space):", sz_buf, sizeof(sz_buf), false, false)) {
        uint64_t mib = 0;
        if (parse_size_mib(sz_buf, &mib)) {
            p->size_mib = mib;
        } else {
            tui_modal_message("Invalid Size", "Use a number with optional M/G suffix, or 0 for remaining.", NULL, true);
        }
    }

    if (p->role != PART_ESP && p->role != PART_ROOT && p->fs != FS_SWAP) {
        char mnt[64];
        snprintf(mnt, sizeof(mnt), "%s", p->mountpoint);
        if (tui_prompt_text("Mountpoint", "Mountpoint (must start with '/', e.g. /home or /var):", mnt, sizeof(mnt), false, false)) {
            snprintf(p->mountpoint, sizeof(p->mountpoint), "%s", mnt);
        }
    }

    char lbl[32];
    snprintf(lbl, sizeof(lbl), "%s", p->label);
    if (tui_prompt_text("Partition Label", "GPT / filesystem label:", lbl, sizeof(lbl), false, false)) {
        snprintf(p->label, sizeof(p->label), "%s", lbl);
    }
}

static void add_partition_dialog(installer_cfg_t *cfg) {
    if (cfg->part_count >= MAX_PARTITIONS) {
        tui_modal_message("Maximum Partitions", "Up to 8 partitions are supported.", NULL, true);
        return;
    }

    const char *roles[] = {
        "/home      (ext4  - User home directories)",
        "/var       (ext4  - Variable & package data)",
        "swap       (swap  - Virtual memory / swap)",
        "Custom     (ext4  - Custom mountpoint)",
    };
    int sel = 0;
    int w = 58, h = 11;
    while (1) {
        int x = (g_term_cols - w) / 2;
        int y = (g_term_rows - h) / 2;
        tui_draw_chrome("Add Partition", "Up/Down: Move   Enter: Select   Esc: Cancel");
        tui_draw_box(y, x, h, w, "Choose Partition Role", C_BORDER, C_PANEL);
        for (int i = 0; i < 4; i++) {
            bool active = (i == sel);
            uint8_t bg = active ? C_SEL_BG : C_PANEL;
            uint8_t fg = active ? C_SEL_FG : C_TEXT;
            tui_fill_rect(y + 3 + i, x + 2, 1, w - 4, fg, bg, ' ');
            tui_put_str_n(y + 3 + i, x + 4, w - 8, fg, bg, active ? 1 : 0, roles[i]);
        }
        tui_flush();

        int k = tui_read_key();
        if (k == KEY_ESC) return;
        if (k == KEY_UP || k == 'k') sel = (sel + 3) % 4;
        else if (k == KEY_DOWN || k == 'j') sel = (sel + 1) % 4;
        else if (k == KEY_ENTER) break;
    }

    part_entry_t np;
    memset(&np, 0, sizeof(np));
    if (sel == 0) {
        np.role = PART_HOME;
        np.fs = FS_EXT4;
        snprintf(np.mountpoint, sizeof(np.mountpoint), "/home");
        snprintf(np.label, sizeof(np.label), "AvoryOS-Home");
        np.size_mib = 0;
    } else if (sel == 1) {
        np.role = PART_VAR;
        np.fs = FS_EXT4;
        snprintf(np.mountpoint, sizeof(np.mountpoint), "/var");
        snprintf(np.label, sizeof(np.label), "AvoryOS-Var");
        np.size_mib = 4096;
    } else if (sel == 2) {
        np.role = PART_SWAP;
        np.fs = FS_SWAP;
        snprintf(np.mountpoint, sizeof(np.mountpoint), "swap");
        snprintf(np.label, sizeof(np.label), "AvoryOS-Swap");
        np.size_mib = 2048;
    } else {
        np.role = PART_CUSTOM;
        np.fs = FS_EXT4;
        snprintf(np.mountpoint, sizeof(np.mountpoint), "/data");
        snprintf(np.label, sizeof(np.label), "AvoryOS-Data");
        np.size_mib = 4096;
        if (!tui_prompt_text("Custom Mountpoint", "Mountpoint (e.g. /data, /opt):", np.mountpoint, sizeof(np.mountpoint), false, false)) {
            return;
        }
    }

    char sz_buf[32];
    snprintf(sz_buf, sizeof(sz_buf), "%lluM", (unsigned long long)np.size_mib);
    if (np.size_mib == 0) snprintf(sz_buf, sizeof(sz_buf), "0");
    if (!tui_prompt_text("Partition Size", "Size (e.g. 20G, 4096M, or 0 for remaining space):", sz_buf, sizeof(sz_buf), false, false)) {
        return;
    }
    if (!parse_size_mib(sz_buf, &np.size_mib)) {
        tui_modal_message("Invalid Size", "Use a number with optional M/G suffix, or 0 for remaining.", NULL, true);
        return;
    }

    if (np.size_mib == 0) {
        for (int i = 0; i < cfg->part_count; i++) {
            if (cfg->parts[i].size_mib == 0) cfg->parts[i].size_mib = 4096;
        }
    }
    cfg->parts[cfg->part_count++] = np;
    cfg->part_scheme = SCHEME_CUSTOM;
}

static void edit_partitions(installer_cfg_t *cfg, const disk_t *d) {
    if (cfg->part_count <= 0) {
        disks_init_preset(SCHEME_SINGLE_ROOT, d->size_bytes, cfg->parts, &cfg->part_count);
    }

    int sel = 0;
    uint64_t disk_mib = d->size_bytes / (1024ULL * 1024ULL);
    uint64_t usable_mib = (disk_mib > 2) ? (disk_mib - 2) : 0;

    while (1) {
        int w = g_term_cols - 6;
        if (w > 86) w = 86;
        int h = MAX_PARTITIONS + 10;
        int x = (g_term_cols - w) / 2;
        int y = (g_term_rows - h) / 2;

        uint64_t fixed_mib = 0;
        bool has_fill = false;
        for (int i = 0; i < cfg->part_count; i++) {
            if (cfg->parts[i].size_mib == 0) has_fill = true;
            else fixed_mib += cfg->parts[i].size_mib;
        }
        uint64_t free_mib = has_fill ? 0 : ((usable_mib > fixed_mib) ? (usable_mib - fixed_mib) : 0);

        tui_draw_chrome("Partition Editor", "a: Add  e/Enter: Edit  d: Delete  f: Fill Rest  s: Save  Esc: Back");
        tui_draw_box(y, x, h, w, "Divide Target Disk", C_BORDER, C_PANEL);

        char disk_sz[24];
        disks_format_size(d->size_bytes, disk_sz, sizeof(disk_sz));
        tui_printf(y + 2, x + 3, C_ACCENT, C_PANEL, 1,
                   "Disk: %s (%s)   Fixed: %llu MiB   Unallocated: %llu MiB%s",
                   d->dev_path, disk_sz, (unsigned long long)fixed_mib,
                   (unsigned long long)free_mib, has_fill ? " (auto-fill)" : "");

        tui_put_str(y + 4, x + 3, C_MUTED, C_PANEL, 1,
                    "#   Mountpoint         FS      Label             Size");

        for (int i = 0; i < cfg->part_count; i++) {
            part_entry_t *p = &cfg->parts[i];
            bool active = (i == sel);
            uint8_t bg = active ? C_SEL_BG : C_PANEL;
            uint8_t fg = active ? C_SEL_FG : C_TEXT;

            char sz_str[32];
            if (p->size_mib == 0) {
                uint64_t rem = (usable_mib > fixed_mib) ? (usable_mib - fixed_mib) : 0;
                snprintf(sz_str, sizeof(sz_str), "Rest (~%lluM)", (unsigned long long)rem);
            } else if (p->size_mib >= 1024 && (p->size_mib % 1024) == 0) {
                snprintf(sz_str, sizeof(sz_str), "%llu GiB", (unsigned long long)(p->size_mib / 1024));
            } else {
                snprintf(sz_str, sizeof(sz_str), "%llu MiB", (unsigned long long)p->size_mib);
            }

            tui_fill_rect(y + 5 + i, x + 2, 1, w - 4, fg, bg, ' ');
            tui_printf(y + 5 + i, x + 3, fg, bg, active ? 1 : 0,
                       "%-2d  %-17.17s  %-6s  %-16.16s  %s",
                       i + 1, p->mountpoint, disks_fs_name(p->fs), p->label, sz_str);
        }

        tui_put_str(y + h - 2, x + 3, C_MUTED, C_PANEL, 0,
                    "Note: ESP (/boot/efi, >=128M) and Root (/) are required.");
        tui_flush();

        int k = tui_read_key();
        if (k == KEY_UP || k == 'k') {
            if (cfg->part_count > 0) sel = (sel - 1 + cfg->part_count) % cfg->part_count;
        } else if (k == KEY_DOWN || k == 'j') {
            if (cfg->part_count > 0) sel = (sel + 1) % cfg->part_count;
        } else if (k == 'a') {
            add_partition_dialog(cfg);
            if (sel >= cfg->part_count) sel = cfg->part_count - 1;
        } else if ((k == 'e' || k == KEY_ENTER) && cfg->part_count > 0) {
            edit_single_part(&cfg->parts[sel]);
            cfg->part_scheme = SCHEME_CUSTOM;
        } else if (k == 'd' && cfg->part_count > 1) {
            for (int i = sel; i < cfg->part_count - 1; i++) {
                cfg->parts[i] = cfg->parts[i + 1];
            }
            cfg->part_count--;
            if (sel >= cfg->part_count) sel = cfg->part_count - 1;
            cfg->part_scheme = SCHEME_CUSTOM;
        } else if (k == 'f' && cfg->part_count > 0) {
            for (int i = 0; i < cfg->part_count; i++) {
                if (i != sel && cfg->parts[i].size_mib == 0) cfg->parts[i].size_mib = 4096;
            }
            cfg->parts[sel].size_mib = 0;
            cfg->part_scheme = SCHEME_CUSTOM;
        } else if (k == 'r') {
            cfg->part_scheme = SCHEME_SINGLE_ROOT;
            disks_init_preset(SCHEME_SINGLE_ROOT, d->size_bytes, cfg->parts, &cfg->part_count);
            sel = 0;
        } else if (k == 's' || k == KEY_ESC) {
            char err[160] = {0};
            if (disks_validate_parts(cfg->parts, cfg->part_count, d->size_bytes, 0, true, err, sizeof(err))) {
                return;
            }
            if (k == KEY_ESC) {
                if (tui_modal_confirm("Invalid Layout", err, "Reset to default layout and exit?", true)) {
                    cfg->part_scheme = SCHEME_SINGLE_ROOT;
                    cfg->wipe_disk = true;
                    disks_init_preset(SCHEME_SINGLE_ROOT, d->size_bytes, cfg->parts, &cfg->part_count);
                    return;
                }
            } else {
                tui_modal_message("Invalid Partition Layout", err, NULL, true);
            }
        }
    }
}

static void assign_existing_role(part_entry_t *p) {
    const char *roles[] = {
        "/          (ext4  - AvoryOS Root, will format)",
        "/boot/efi  (fat32 - EFI System Partition)",
        "/home      (ext4  - User home directories)",
        "/var       (ext4  - Variable & package data)",
        "swap       (swap  - Swap partition)",
        "Custom     (ext4  - Custom mountpoint)",
        "[ignore]   (leave partition untouched)",
    };
    int sel = 0;
    int w = 60, h = 13;
    while (1) {
        int x = (g_term_cols - w) / 2;
        int y = (g_term_rows - h) / 2;
        tui_draw_chrome("Assign Partition Role", "Up/Down: Move   Enter: Select   Esc: Cancel");
        tui_draw_box(y, x, h, w, p->dev_path[0] ? p->dev_path : "Assign Role", C_BORDER, C_PANEL);
        for (int i = 0; i < 7; i++) {
            bool active = (i == sel);
            uint8_t bg = active ? C_SEL_BG : C_PANEL;
            uint8_t fg = active ? C_SEL_FG : C_TEXT;
            tui_fill_rect(y + 3 + i, x + 2, 1, w - 4, fg, bg, ' ');
            tui_put_str_n(y + 3 + i, x + 4, w - 8, fg, bg, active ? 1 : 0, roles[i]);
        }
        tui_flush();

        int k = tui_read_key();
        if (k == KEY_ESC) return;
        if (k == KEY_UP || k == 'k') sel = (sel + 6) % 7;
        else if (k == KEY_DOWN || k == 'j') sel = (sel + 1) % 7;
        else if (k == KEY_ENTER) break;
    }

    if (sel == 0) {
        p->role = PART_ROOT; p->fs = FS_EXT4; p->format = true;
        snprintf(p->mountpoint, sizeof(p->mountpoint), "/");
        if (!p->label[0]) snprintf(p->label, sizeof(p->label), "AvoryOS-Root");
    } else if (sel == 1) {
        p->role = PART_ESP; p->fs = FS_FAT32;
        p->format = (strcmp(p->cur_fs, "fat32") != 0 && strcmp(p->cur_fs, "vfat") != 0);
        snprintf(p->mountpoint, sizeof(p->mountpoint), "/boot/efi");
        if (!p->label[0]) snprintf(p->label, sizeof(p->label), "EFI");
    } else if (sel == 2) {
        p->role = PART_HOME; p->fs = FS_EXT4; p->format = false;
        snprintf(p->mountpoint, sizeof(p->mountpoint), "/home");
        if (!p->label[0]) snprintf(p->label, sizeof(p->label), "AvoryOS-Home");
    } else if (sel == 3) {
        p->role = PART_VAR; p->fs = FS_EXT4; p->format = false;
        snprintf(p->mountpoint, sizeof(p->mountpoint), "/var");
        if (!p->label[0]) snprintf(p->label, sizeof(p->label), "AvoryOS-Var");
    } else if (sel == 4) {
        p->role = PART_SWAP; p->fs = FS_SWAP; p->format = true;
        snprintf(p->mountpoint, sizeof(p->mountpoint), "swap");
        if (!p->label[0]) snprintf(p->label, sizeof(p->label), "AvoryOS-Swap");
    } else if (sel == 5) {
        char mnt[64] = "/data";
        if (!tui_prompt_text("Custom Mountpoint", "Mountpoint (e.g. /data, /opt):", mnt, sizeof(mnt), false, false)) return;
        p->role = PART_CUSTOM; p->fs = FS_EXT4; p->format = false;
        snprintf(p->mountpoint, sizeof(p->mountpoint), "%s", mnt);
    } else {
        p->role = PART_NONE; p->fs = FS_OTHER; p->format = false;
        snprintf(p->mountpoint, sizeof(p->mountpoint), "[ignore]");
    }
}

static void create_in_free_gap(installer_cfg_t *cfg, int idx) {
    if (cfg->part_count >= MAX_PARTITIONS) {
        tui_modal_message("Maximum Entries", "Too many partition entries.", NULL, true);
        return;
    }
    part_entry_t gap = cfg->parts[idx];
    part_entry_t np;
    memset(&np, 0, sizeof(np));
    np.start_mib = gap.start_mib;
    np.size_mib = gap.size_mib;
    np.existing = false;
    np.is_free = false;
    np.format = true;
    snprintf(np.cur_fs, sizeof(np.cur_fs), "new");

    assign_existing_role(&np);
    if (np.role == PART_NONE) return;
    np.format = true; /* newly created partitions must be formatted */

    char sz_buf[32];
    snprintf(sz_buf, sizeof(sz_buf), "%lluM", (unsigned long long)gap.size_mib);
    if (!tui_prompt_text("New Partition Size", "Size (e.g. 8G, 4096M; max available in gap):", sz_buf, sizeof(sz_buf), false, false)) return;
    uint64_t req_mib = 0;
    if (!parse_size_mib(sz_buf, &req_mib)) {
        tui_modal_message("Invalid Size", "Use a number with optional M/G suffix.", NULL, true);
        return;
    }
    if (req_mib == 0 || req_mib > gap.size_mib) req_mib = gap.size_mib;
    if (req_mib < 64) {
        tui_modal_message("Too Small", "Partition must be at least 64 MiB.", NULL, true);
        return;
    }
    np.size_mib = req_mib;

    if (req_mib + 16 <= gap.size_mib) {
        /* Split the free gap: new partition followed by remaining free space */
        for (int i = cfg->part_count; i > idx + 1; i--) {
            cfg->parts[i] = cfg->parts[i - 1];
        }
        cfg->parts[idx] = np;
        cfg->parts[idx + 1] = gap;
        cfg->parts[idx + 1].start_mib = gap.start_mib + req_mib;
        cfg->parts[idx + 1].size_mib = gap.size_mib - req_mib;
        cfg->part_count++;
    } else {
        cfg->parts[idx] = np;
    }
}

static void edit_existing_partitions(installer_cfg_t *cfg, const disk_t *d) {
    int sel = 0;
    while (1) {
        int w = g_term_cols - 4;
        if (w > 94) w = 94;
        int visible = cfg->part_count < 12 ? cfg->part_count : 12;
        if (visible < 4) visible = 4;
        int h = visible + 9;
        int x = (g_term_cols - w) / 2;
        int y = (g_term_rows - h) / 2;

        tui_draw_chrome("Existing Partitions",
                        "Enter/m: Role  Space/f: Format  a: Add  z: Shrink  d: Del  r: Rescan  s: Save");
        tui_draw_box(y, x, h, w, "Non-Destructive Partition Assignment", C_BORDER, C_PANEL);

        char disk_sz[24];
        disks_format_size(d->size_bytes, disk_sz, sizeof(disk_sz));
        tui_printf(y + 2, x + 3, C_ACCENT, C_PANEL, 1,
                   "Disk: %s (%s) — Existing partitions preserved unless marked Format=Yes",
                   d->dev_path, disk_sz);

        tui_put_str(y + 4, x + 3, C_MUTED, C_PANEL, 1,
                    "Part/Gap         Cur FS    Mountpoint       Format  Size");

        for (int i = 0; i < cfg->part_count && i < visible; i++) {
            part_entry_t *p = &cfg->parts[i];
            bool active = (i == sel);
            uint8_t bg = active ? C_SEL_BG : C_PANEL;
            uint8_t fg = active ? C_SEL_FG : (p->is_free ? C_OK : (p->role == PART_NONE ? C_MUTED : C_TEXT));

            const char *name = p->is_free ? "[FREE SPACE]" :
                               (p->existing ? p->dev_path : "[NEW PART]");
            const char *fmt_s = p->is_free ? "-" :
                                (p->role == PART_NONE ? "-" : (p->format ? "Yes" : "No"));

            char sz_str[32];
            if (p->size_mib >= 1024 && (p->size_mib % 1024) == 0) {
                snprintf(sz_str, sizeof(sz_str), "%llu GiB", (unsigned long long)(p->size_mib / 1024));
            } else {
                snprintf(sz_str, sizeof(sz_str), "%llu MiB", (unsigned long long)p->size_mib);
            }

            tui_fill_rect(y + 5 + i, x + 2, 1, w - 4, fg, bg, ' ');
            tui_printf(y + 5 + i, x + 3, fg, bg, active ? 1 : 0,
                       "%-15.15s  %-8.8s  %-15.15s  %-6s  %s",
                       name, p->cur_fs, p->mountpoint, fmt_s, sz_str);
        }

        tui_put_str(y + h - 2, x + 3, C_MUTED, C_PANEL, 0,
                    "Assign ESP (/boot/efi) and Root (/, Format=Yes). Press 's' to save.");
        tui_flush();

        int k = tui_read_key();
        if (k == KEY_UP || k == 'k') {
            if (cfg->part_count > 0) sel = (sel - 1 + cfg->part_count) % cfg->part_count;
        } else if (k == KEY_DOWN || k == 'j') {
            if (cfg->part_count > 0) sel = (sel + 1) % cfg->part_count;
        } else if ((k == KEY_ENTER || k == 'm') && cfg->part_count > 0) {
            if (cfg->parts[sel].is_free) {
                create_in_free_gap(cfg, sel);
            } else {
                assign_existing_role(&cfg->parts[sel]);
                if (!cfg->parts[sel].existing) cfg->parts[sel].format = true;
            }
        } else if (k == 'a' && cfg->part_count > 0) {
            if (!cfg->parts[sel].is_free) {
                tui_modal_message("Select Free Space", "Move cursor to a [FREE SPACE] row and press 'a'.", NULL, false);
            } else {
                create_in_free_gap(cfg, sel);
            }
        } else if ((k == KEY_SPACE || k == 'f') && cfg->part_count > 0) {
            part_entry_t *p = &cfg->parts[sel];
            if (!p->is_free && p->role != PART_NONE && p->existing) {
                if (p->role == PART_ROOT) {
                    tui_modal_message("Format Required", "Root (/) partition must be formatted.", NULL, false);
                } else {
                    p->format = !p->format;
                }
            }
        } else if (k == 'z' && cfg->part_count > 0) {
            part_entry_t *p = &cfg->parts[sel];
            if (!p->existing || p->is_free) {
                tui_modal_message("Select Existing Partition", "Move cursor to an existing partition to shrink it.", NULL, false);
            } else {
                char sz_buf[32] = {0};
                snprintf(sz_buf, sizeof(sz_buf), "%lluM", (unsigned long long)(p->size_mib / 2));
                if (tui_prompt_text("Shrink Partition", "New smaller size (e.g. 4G, 4096M):", sz_buf, sizeof(sz_buf), false, false)) {
                    uint64_t new_mib = 0;
                    if (!parse_size_mib(sz_buf, &new_mib) || new_mib == 0) {
                        tui_modal_message("Invalid Size", "Use a number with optional M/G suffix.", NULL, true);
                    } else {
                        char msg[128];
                        snprintf(msg, sizeof(msg), "Shrink %s from %llu MiB to %llu MiB?",
                                 p->dev_path, (unsigned long long)p->size_mib, (unsigned long long)new_mib);
                        if (tui_modal_confirm("Confirm Partition Shrink", msg, "A [FREE SPACE] gap will be created after it.", true)) {
                            char err[160] = {0};
                            if (disks_shrink_partition(d, p, new_mib, err, sizeof(err)) != 0) {
                                tui_modal_message("Shrink Failed", err, NULL, true);
                            } else {
                                cfg->part_count = disks_scan_partitions(d, cfg->parts, MAX_PARTITIONS);
                                if (sel >= cfg->part_count) sel = cfg->part_count > 0 ? cfg->part_count - 1 : 0;
                            }
                        }
                    }
                }
            }
        } else if (k == 'd' && cfg->part_count > 0) {
            part_entry_t *p = &cfg->parts[sel];
            if (p->existing) {
                if (tui_modal_confirm("Delete Existing Partition",
                                      "PERMANENTLY delete this partition from the disk?",
                                      p->dev_path, true)) {
                    if (disks_delete_partition(d, p->part_num) == 0) {
                        cfg->part_count = disks_scan_partitions(d, cfg->parts, MAX_PARTITIONS);
                        if (sel >= cfg->part_count) sel = cfg->part_count > 0 ? cfg->part_count - 1 : 0;
                    }
                }
            } else if (!p->is_free) {
                /* Revert uncommitted new partition back to free space */
                p->is_free = true;
                p->role = PART_NONE;
                p->fs = FS_OTHER;
                p->format = false;
                snprintf(p->cur_fs, sizeof(p->cur_fs), "free");
                snprintf(p->mountpoint, sizeof(p->mountpoint), "[FREE SPACE]");
            }
        } else if (k == 'r') {
            cfg->part_count = disks_scan_partitions(d, cfg->parts, MAX_PARTITIONS);
            if (sel >= cfg->part_count) sel = cfg->part_count > 0 ? cfg->part_count - 1 : 0;
        } else if (k == 's' || k == KEY_ESC) {
            char err[160] = {0};
            if (disks_validate_parts(cfg->parts, cfg->part_count, d->size_bytes, 0, false, err, sizeof(err))) {
                return;
            }
            if (k == KEY_ESC) {
                if (tui_modal_confirm("Incomplete Assignment", err, "Reset to Guided Single Root and exit?", true)) {
                    cfg->part_scheme = SCHEME_SINGLE_ROOT;
                    cfg->wipe_disk = true;
                    disks_init_preset(SCHEME_SINGLE_ROOT, d->size_bytes, cfg->parts, &cfg->part_count);
                    return;
                }
            } else {
                tui_modal_message("Invalid Partition Assignment", err, NULL, true);
            }
        }
    }
}

static void pick_partition_scheme(installer_cfg_t *cfg, const disk_t *d) {
    int sel = (int)cfg->part_scheme;
    const int count = 6;
    int w = 68, h = 14;
    while (1) {
        int x = (g_term_cols - w) / 2;
        int y = (g_term_rows - h) / 2;
        tui_draw_chrome("Partition Layout", "Up/Down/j/k: Move   Enter: Select   e: Customize   Esc: Back");
        tui_draw_box(y, x, h, w, "Choose Partition Division Scheme", C_BORDER, C_PANEL);

        char sz[24];
        disks_format_size(d->size_bytes, sz, sizeof(sz));
        tui_printf(y + 2, x + 4, C_MUTED, C_PANEL, 0, "Target: %s (%s)", d->dev_path, sz);

        for (int i = 0; i < count; i++) {
            bool active = (i == sel);
            uint8_t bg = active ? C_SEL_BG : C_PANEL;
            uint8_t fg = active ? C_SEL_FG : C_TEXT;
            tui_fill_rect(y + 4 + i, x + 2, 1, w - 4, fg, bg, ' ');
            tui_put_str_n(y + 4 + i, x + 4, w - 8, fg, bg, active ? 1 : 0,
                          disks_scheme_name((part_scheme_t)i));
        }
        tui_flush();

        int k = tui_read_key();
        if (k == KEY_ESC) return;
        if (k == KEY_UP || k == 'k') sel = (sel - 1 + count) % count;
        else if (k == KEY_DOWN || k == 'j') sel = (sel + 1) % count;
        else if (k == 'e') {
            if (sel == SCHEME_EXISTING) {
                cfg->part_scheme = SCHEME_EXISTING;
                cfg->wipe_disk = false;
                cfg->part_count = disks_scan_partitions(d, cfg->parts, MAX_PARTITIONS);
                if (cfg->part_count == 0) {
                    tui_modal_message("No Partitions", "No partition table found. Use a Guided or Custom wipe scheme.", NULL, true);
                    continue;
                }
                edit_existing_partitions(cfg, d);
                return;
            }
            if (sel < SCHEME_CUSTOM) {
                disks_init_preset((part_scheme_t)sel, d->size_bytes, cfg->parts, &cfg->part_count);
            }
            cfg->part_scheme = SCHEME_CUSTOM;
            cfg->wipe_disk = true;
            edit_partitions(cfg, d);
            return;
        } else if (k == KEY_ENTER) {
            cfg->part_scheme = (part_scheme_t)sel;
            if (cfg->part_scheme == SCHEME_EXISTING) {
                cfg->wipe_disk = false;
                cfg->part_count = disks_scan_partitions(d, cfg->parts, MAX_PARTITIONS);
                if (cfg->part_count == 0) {
                    tui_modal_message("No Partitions", "No partition table found. Use a Guided or Custom wipe scheme.", NULL, true);
                    continue;
                }
                edit_existing_partitions(cfg, d);
            } else if (cfg->part_scheme == SCHEME_CUSTOM) {
                cfg->wipe_disk = true;
                edit_partitions(cfg, d);
            } else {
                cfg->wipe_disk = true;
                disks_init_preset(cfg->part_scheme, d->size_bytes, cfg->parts, &cfg->part_count);
            }
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
            bool changed = (cfg->disk_idx != sel);
            cfg->disk_idx = sel;
            if (changed || cfg->part_count <= 0) {
                if (cfg->part_scheme == SCHEME_CUSTOM) cfg->part_scheme = SCHEME_SINGLE_ROOT;
                disks_init_preset(cfg->part_scheme, g_disks[sel].size_bytes, cfg->parts, &cfg->part_count);
            }
            pick_partition_scheme(cfg, &g_disks[sel]);
            return;
        }
    }
}

static bool desktop_available(desktop_choice_t d) {
    switch (d) {
        case DE_PLASMA:
            return access("/usr/bin/startplasma-x11", X_OK) == 0 ||
                   access("/usr/bin/startplasma-wayland", X_OK) == 0;
        case DE_XFCE4:   return access("/usr/bin/startxfce4", X_OK) == 0;
        case DE_ICEWM:   return access("/usr/bin/icewm", X_OK) == 0;
        case DE_CONSOLE: return true;
        default:         return false;
    }
}

static void pick_desktop(installer_cfg_t *cfg) {
    desktop_choice_t avail[4];
    int count = 0;
    int sel = 0;
    for (int i = 0; i < 4; i++) {
        desktop_choice_t d = (desktop_choice_t)i;
        if (desktop_available(d)) {
            if (d == cfg->desktop) sel = count;
            avail[count++] = d;
        }
    }
    int w = 56, h = count + 7;
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
            tui_printf(y + 3 + i, x + 4, fg, bg, active ? 1 : 0, "%s", desktop_name(avail[i]));
        }
        tui_flush();

        int k = tui_read_key();
        if (k == KEY_ESC) return;
        if (k == KEY_UP || k == 'k') sel = (sel - 1 + count) % count;
        else if (k == KEY_DOWN || k == 'j') sel = (sel + 1) % count;
        else if (k == KEY_ENTER) {
            cfg->desktop = avail[sel];
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
    char orig_uname[32];
    snprintf(orig_uname, sizeof(orig_uname), "%s", uname);
    for (size_t i = 0; uname[i]; i++) {
        if (!isalnum((unsigned char)uname[i]) && uname[i] != '_' && uname[i] != '-') {
            tui_modal_message("Invalid Username", "Use only letters, digits, '_' or '-'.", NULL, true);
            return;
        }
        uname[i] = (char)tolower((unsigned char)uname[i]);
    }
    if (strcmp(fname, "AvoryOS User") == 0 && strcmp(orig_uname, "avory") != 0) {
        snprintf(fname, sizeof(fname), "%s", orig_uname);
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
    if (cfg->part_count <= 0) {
        cfg->wipe_disk = true;
        disks_init_preset(cfg->part_scheme, d->size_bytes, cfg->parts, &cfg->part_count);
    }

    char sz[24], warn_line[128], layout_line[160], summary[128];
    disks_format_size(d->size_bytes, sz, sizeof(sz));
    if (cfg->wipe_disk) {
        snprintf(warn_line, sizeof(warn_line), "ALL DATA ON %s (%s, %s) WILL BE ERASED!", d->dev_path, sz, d->model);
    } else {
        snprintf(warn_line, sizeof(warn_line), "Non-destructive install on %s (%s): only selected/formatted parts change.", d->dev_path, sz);
    }
    disks_format_part_summary(cfg->parts, cfg->part_count, summary, sizeof(summary));
    snprintf(layout_line, sizeof(layout_line), "Layout: %s", summary);

    const char *title = cfg->dry_run ? "Confirm Dry-Run" :
                        (cfg->wipe_disk ? "Confirm Disk Wipe" : "Confirm Non-Destructive Install");
    if (!tui_modal_confirm(title,
                           warn_line,
                           layout_line,
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
    cfg.wipe_disk = true;
    snprintf(cfg.username, sizeof(cfg.username), "avory");
    snprintf(cfg.fullname, sizeof(cfg.fullname), "AvoryOS User");
    if (desktop_available(DE_PLASMA)) {
        cfg.desktop = DE_PLASMA;
    } else if (desktop_available(DE_XFCE4)) {
        cfg.desktop = DE_XFCE4;
    } else if (desktop_available(DE_ICEWM)) {
        cfg.desktop = DE_ICEWM;
    } else {
        cfg.desktop = DE_CONSOLE;
    }

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
        "Partition layout",
        "Hostname",
        "Root password",
        "User account",
        "Desktop environment",
        "Install",
        "Abort",
    };
    const int n_items = 10;
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
            int ry = y + 2 + i + (i >= 8 ? 1 : 0);
            bool active = (i == sel);
            uint8_t bg = active ? C_SEL_BG : C_PANEL;
            uint8_t fg = active ? C_SEL_FG : C_TEXT;

            if (i == 8) {
                for (int c = x + 2; c < x + w - 2; c++) {
                    tui_put_cell(y + 10, c, C_BORDER, C_PANEL, 0, 0x2500);
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
                if (cfg.disk_idx >= 0 && cfg.disk_idx < g_disk_count) {
                    disks_format_part_summary(cfg.parts, cfg.part_count, val, sizeof(val));
                    val_fg = active ? C_SEL_FG : C_OK;
                } else {
                    snprintf(val, sizeof(val), "[select disk first]");
                    val_fg = active ? C_SEL_FG : C_MUTED;
                }
            } else if (i == 4) {
                snprintf(val, sizeof(val), "%s", cfg.hostname);
                val_fg = active ? C_SEL_FG : C_OK;
            } else if (i == 5) {
                snprintf(val, sizeof(val), "%s", cfg.root_password[0] ? "[password set]" : "[not set]");
                val_fg = active ? C_SEL_FG : (cfg.root_password[0] ? C_OK : C_MUTED);
            } else if (i == 6) {
                snprintf(val, sizeof(val), "%s%s", cfg.username,
                         cfg.user_password[0] ? " (password set)" : " (no password)");
                val_fg = active ? C_SEL_FG : C_OK;
            } else if (i == 7) {
                snprintf(val, sizeof(val), "%s", desktop_name(cfg.desktop));
                val_fg = active ? C_SEL_FG : C_OK;
            } else if (i == 8) {
                fg = active ? C_SEL_FG : C_ACCENT;
            } else if (i == 9) {
                fg = active ? C_SEL_FG : C_ERR;
            }

            tui_put_str(ry, x + 4, fg, bg, (active || i >= 8) ? 1 : 0, items[i]);
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
                case 3:
                    if (cfg.disk_idx < 0 || cfg.disk_idx >= g_disk_count) {
                        tui_modal_message("No Disk Selected", "Please select a target disk first.", NULL, true);
                    } else {
                        pick_partition_scheme(&cfg, &g_disks[cfg.disk_idx]);
                    }
                    break;
                case 4: tui_prompt_text("Hostname", "System hostname:", cfg.hostname, sizeof(cfg.hostname), false, false); break;
                case 5: edit_root_password(&cfg); break;
                case 6: edit_user_account(&cfg); break;
                case 7: pick_desktop(&cfg); break;
                case 8:
                    if (run_install_flow(&cfg)) goto done;
                    break;
                case 9: goto done;
            }
        }
    }

done:
    tui_restore();
    return 0;
}
