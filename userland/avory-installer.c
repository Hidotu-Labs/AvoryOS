/*
 * userland/avory-installer.c — Native TTY Installer for AvoryOS
 * Inspired by FreeBSD bsdinstall and Ubuntu Server subiquity.
 *
 * Implements a full ANSI/VT100 interactive text-mode UI wizard:
 *  - System Welcome & Hardware Overview
 *  - Keyboard Layout / Keymap Selection
 *  - Network Setup (DHCP / Static)
 *  - Guided GPT / Entire Disk Partitioning & Disk Discovery
 *  - Confirmation of Destructive Changes
 *  - Component / Distribution Sets Selection
 *  - Identity / User Account & Root Password Setup
 *  - Live Progress Display with animated gauge
 *  - Bootloader (Limine UEFI + BIOS) deployment
 *  - Reboot / Shell Handoff
 */

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

#define INSTALLER_VERSION "1.0"
#define MAX_DISKS 16
#define TARGET_MOUNT "/mnt/target"

/* Key definitions */
enum {
    KEY_NONE = 0,
    KEY_UP = 1000,
    KEY_DOWN,
    KEY_LEFT,
    KEY_RIGHT,
    KEY_PAGEUP,
    KEY_PAGEDOWN,
    KEY_HOME,
    KEY_END,
    KEY_TAB,
    KEY_BACKTAB,
    KEY_ENTER,
    KEY_BACKSPACE,
    KEY_SPACE,
    KEY_ESC,
    KEY_F1,
    KEY_F2,
    KEY_F10
};

/* Wizard steps */
typedef enum {
    STEP_WELCOME = 0,
    STEP_KEYMAP,
    STEP_NETWORK,
    STEP_DISK_SELECT,
    STEP_CONFIRM_DISK,
    STEP_COMPONENTS,
    STEP_IDENTITY,
    STEP_INSTALLING,
    STEP_COMPLETE,
    STEP_EXIT
} wizard_step_t;

/* Disk details */
typedef struct {
    char name[32];          /* e.g. "nvme0n1", "sda" */
    char dev_path[64];      /* e.g. "/dev/nvme0n1" */
    char model[64];         /* e.g. "QEMU NVMe Controller" */
    uint64_t size_bytes;    /* in bytes */
    char size_str[32];      /* e.g. "20.0 GB" */
    bool is_live_media;     /* true if mounted on / */
} disk_t;

static disk_t g_disks[MAX_DISKS];
static int g_disk_count = 0;
static int g_selected_disk_idx = 0;

/* Configuration state */
static char g_keymap[32] = "us";
static char g_hostname[64] = "avoryos";
static char g_fullname[64] = "AvoryOS User";
static char g_username[32] = "avory";
static char g_password[64] = "";
static char g_password_confirm[64] = "";
static char g_root_password[64] = "";
static char g_root_password_confirm[64] = "";
static bool g_enable_dhcp = true;
static bool g_opt_desktop = true;
static bool g_opt_dev = true;
static bool g_opt_games = true;
static bool g_opt_media = true;
static int g_desktop_choice = 0; /* 0: XFCE4, 1: Plasma, 2: IceWM, 3: Console */

/* Terminal state & Double Buffering */
static struct termios g_orig_termios;
static bool g_raw_mode = false;
static int g_term_rows = 25;
static int g_term_cols = 80;

#define TUI_MAX_ROWS 128
#define TUI_MAX_COLS 256

typedef struct {
    char ch;        /* character (ASCII byte) */
    uint8_t fg;     /* 0: default, 30..37, 90..97 */
    uint8_t bg;     /* 0: default, 40..47, 100..107 */
    uint8_t bold;   /* 0 or 1 */
} tui_cell_t;

static tui_cell_t g_front[TUI_MAX_ROWS][TUI_MAX_COLS];
static tui_cell_t g_back[TUI_MAX_ROWS][TUI_MAX_COLS];
static bool g_force_redraw = true;

static int g_cur_row = 0;
static int g_cur_col = 0;
static uint8_t g_draw_fg = 0;
static uint8_t g_draw_bg = 0;
static uint8_t g_draw_bold = 0;

/* Raw output buffer for batching terminal writes */
static char g_raw_buf[65536];
static size_t g_raw_len = 0;

static void tui_raw_write(const char *str, size_t len) {
    if (g_raw_len + len < sizeof(g_raw_buf)) {
        memcpy(g_raw_buf + g_raw_len, str, len);
        g_raw_len += len;
    }
}

static void tui_raw_str(const char *str) {
    tui_raw_write(str, strlen(str));
}

static void tui_raw_printf(const char *fmt, ...) {
    char tmp[256];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    if (n > 0) tui_raw_write(tmp, (size_t)n);
}

static void tui_cursor(int r, int c) {
    if (r < 1) r = 1;
    if (c < 1) c = 1;
    g_cur_row = r - 1;
    g_cur_col = c - 1;
}

static void tui_buf_write(const char *str) {
    if (!str) return;
    const char *p = str;
    while (*p) {
        if (*p == '\033' && *(p + 1) == '[') {
            /* Parse ANSI escape sequence */
            p += 2;
            int args[8] = {0};
            int argc = 0;
            while (isdigit(*p) || *p == ';') {
                if (*p == ';') {
                    if (argc < 7) argc++;
                    p++;
                } else {
                    args[argc] = args[argc] * 10 + (*p - '0');
                    p++;
                }
            }
            if (argc < 8) argc++;
            char cmd = *p ? *p++ : '\0';
            if (cmd == 'm') {
                /* SGR formatting */
                if (argc == 1 && args[0] == 0) {
                    g_draw_fg = 0;
                    g_draw_bg = 0;
                    g_draw_bold = 0;
                } else {
                    for (int i = 0; i < argc; i++) {
                        int a = args[i];
                        if (a == 0) {
                            g_draw_fg = 0;
                            g_draw_bg = 0;
                            g_draw_bold = 0;
                        } else if (a == 1) {
                            g_draw_bold = 1;
                        } else if (a == 22) {
                            g_draw_bold = 0;
                        } else if ((a >= 30 && a <= 37) || (a >= 90 && a <= 97)) {
                            g_draw_fg = (uint8_t)a;
                        } else if (a == 39) {
                            g_draw_fg = 0;
                        } else if ((a >= 40 && a <= 47) || (a >= 100 && a <= 107)) {
                            g_draw_bg = (uint8_t)a;
                        } else if (a == 49) {
                            g_draw_bg = 0;
                        }
                    }
                }
            } else if (cmd == 'H' || cmd == 'f') {
                /* Cursor position: \033[r;cH */
                int r = (argc >= 1 && args[0] > 0) ? args[0] : 1;
                int c = (argc >= 2 && args[1] > 0) ? args[1] : 1;
                tui_cursor(r, c);
            }
        } else if (*p == '\n') {
            g_cur_row++;
            g_cur_col = 0;
            p++;
        } else if (*p == '\r') {
            g_cur_col = 0;
            p++;
        } else {
            /* Normal character cell */
            if (g_cur_row >= 0 && g_cur_row < g_term_rows &&
                g_cur_col >= 0 && g_cur_col < g_term_cols) {
                tui_cell_t *cell = &g_back[g_cur_row][g_cur_col];
                cell->ch = *p;
                cell->fg = g_draw_fg;
                cell->bg = g_draw_bg;
                cell->bold = g_draw_bold;
            }
            g_cur_col++;
            p++;
        }
    }
}

static void tui_buf_printf(const char *fmt, ...) {
    char tmp[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    tui_buf_write(tmp);
}

/* Dirty Rectangle / Cell Matrix Flush */
static void tui_flush(void) {
    int hw_r = -1, hw_c = -1;
    uint8_t hw_fg = 0xFF, hw_bg = 0xFF, hw_bold = 0xFF;

    g_raw_len = 0;

    for (int r = 0; r < g_term_rows; r++) {
        /* Find dirty span in row r */
        int first_dirty = -1, last_dirty = -1;
        for (int c = 0; c < g_term_cols; c++) {
            tui_cell_t *b = &g_back[r][c];
            tui_cell_t *f = &g_front[r][c];
            if (g_force_redraw ||
                b->ch != f->ch || b->fg != f->fg ||
                b->bg != f->bg || b->bold != f->bold) {
                if (first_dirty == -1) first_dirty = c;
                last_dirty = c;
            }
        }

        if (first_dirty == -1) continue; /* Row is clean, skip! */

        for (int c = first_dirty; c <= last_dirty; c++) {
            tui_cell_t *b = &g_back[r][c];
            tui_cell_t *f = &g_front[r][c];

            /* If unchanged inside span and not forcing redraw, skip */
            if (!g_force_redraw &&
                b->ch == f->ch && b->fg == f->fg &&
                b->bg == f->bg && b->bold == f->bold) {
                continue;
            }

            *f = *b;

            /* Jump cursor if needed */
            if (hw_r != r || hw_c != c) {
                tui_raw_printf("\033[%d;%dH", r + 1, c + 1);
                hw_r = r;
                hw_c = c;
            }

            /* Apply colors & attributes */
            if (b->fg != hw_fg || b->bg != hw_bg || b->bold != hw_bold) {
                tui_raw_str("\033[0m");
                if (b->bold) tui_raw_str("\033[1m");
                if (b->fg) tui_raw_printf("\033[%dm", b->fg);
                if (b->bg) tui_raw_printf("\033[%dm", b->bg);
                hw_fg = b->fg;
                hw_bg = b->bg;
                hw_bold = b->bold;
            }

            char ch = b->ch ? b->ch : ' ';
            tui_raw_write(&ch, 1);
            hw_c++;
        }
    }

    g_force_redraw = false;

    if (g_raw_len > 0) {
        write(STDOUT_FILENO, g_raw_buf, g_raw_len);
        g_raw_len = 0;
    }
}

static void tui_force_redraw(void) {
    memset(g_front, 0xFF, sizeof(g_front));
    g_force_redraw = true;
}

static void tui_update_size(void) {
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0) {
        if (ws.ws_row > 10 && ws.ws_row <= TUI_MAX_ROWS) g_term_rows = ws.ws_row;
        if (ws.ws_col > 30 && ws.ws_col <= TUI_MAX_COLS) g_term_cols = ws.ws_col;
    }
    tui_force_redraw();
}

static void tui_restore_terminal(void);

static void sigwinch_handler(int sig) {
    (void)sig;
    tui_update_size();
}

static void sigint_handler(int sig) {
    (void)sig;
    tui_restore_terminal();
    exit(130);
}

static void tui_init_terminal(void) {
    if (tcgetattr(STDIN_FILENO, &g_orig_termios) < 0) {
        perror("tcgetattr");
    }
    struct termios raw = g_orig_termios;
    raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
    raw.c_oflag &= ~(OPOST);
    raw.c_cflag |= (CS8);
    raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
    g_raw_mode = true;

    signal(SIGWINCH, sigwinch_handler);
    signal(SIGINT, sigint_handler);
    signal(SIGTERM, sigint_handler);

    tui_update_size();

    /* Hide cursor and switch to alternate screen */
    const char *init_seq = "\033[?25l\033[?1049h\033[2J\033[H";
    write(STDOUT_FILENO, init_seq, strlen(init_seq));
    tui_force_redraw();
}

static void tui_restore_terminal(void) {
    if (g_raw_mode) {
        /* Restore screen, reset colors, show cursor */
        const char *restore_seq = "\033[0m\033[?25h\033[?1049l\033[2J\033[H";
        write(STDOUT_FILENO, restore_seq, strlen(restore_seq));
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &g_orig_termios);
        g_raw_mode = false;
    }
}

/* Read one key / escape sequence */
static int tui_read_key(void) {
    char c;
    if (read(STDIN_FILENO, &c, 1) != 1) return KEY_NONE;

    if (c == 27) { /* Escape sequence */
        char seq[8];
        int n = 0;
        /* Set non-blocking to inspect follow-up bytes */
        int flags = fcntl(STDIN_FILENO, F_GETFL, 0);
        fcntl(STDIN_FILENO, F_SETFL, flags | O_NONBLOCK);
        while (n < 7 && read(STDIN_FILENO, &seq[n], 1) == 1) {
            n++;
        }
        fcntl(STDIN_FILENO, F_SETFL, flags);

        if (n == 0) return KEY_ESC;

        if (seq[0] == '[') {
            if (seq[1] == 'A') return KEY_UP;
            if (seq[1] == 'B') return KEY_DOWN;
            if (seq[1] == 'C') return KEY_RIGHT;
            if (seq[1] == 'D') return KEY_LEFT;
            if (seq[1] == 'Z') return KEY_BACKTAB;
            if (seq[1] == 'H') return KEY_HOME;
            if (seq[1] == 'F') return KEY_END;
            if (seq[1] >= '0' && seq[1] <= '9') {
                if (seq[2] == '~') {
                    if (seq[1] == '1' || seq[1] == '7') return KEY_HOME;
                    if (seq[1] == '4' || seq[1] == '8') return KEY_END;
                    if (seq[1] == '5') return KEY_PAGEUP;
                    if (seq[1] == '6') return KEY_PAGEDOWN;
                }
            }
        } else if (seq[0] == 'O') {
            if (seq[1] == 'P') return KEY_F1;
            if (seq[1] == 'Q') return KEY_F2;
        }
        return KEY_ESC;
    }

    if (c == '\r' || c == '\n') return KEY_ENTER;
    if (c == '\t') return KEY_TAB;
    if (c == 127 || c == 8) return KEY_BACKSPACE;
    if (c == ' ') return KEY_SPACE;

    return (unsigned char)c;
}

/* ──────────────────────────────────────────────────────────────────────────
 * Drawing Primitives: FreeBSD / Ubuntu Server Aesthetics
 * ────────────────────────────────────────────────────────────────────────── */

/* Draw a full screen backdrop with FreeBSD Blue and header */
static void tui_draw_backdrop(const char *step_name, int step_num, int total_steps) {
    /* Blue background, white text */
    tui_cursor(1, 1);
    tui_buf_write("\033[44m\033[37m");

    /* Fill background */
    for (int r = 1; r <= g_term_rows; r++) {
        tui_cursor(r, 1);
        for (int c = 1; c <= g_term_cols; c++) {
            tui_buf_write(" ");
        }
    }

    /* Top banner (White on Dark Blue / Cyan) */
    tui_cursor(1, 1);
    tui_buf_write("\033[1;37;44m");
    char title[128];
    snprintf(title, sizeof(title), "  AvoryOS %s Installation Suite", INSTALLER_VERSION);
    tui_buf_write(title);

    char step_info[64];
    if (step_num > 0) {
        snprintf(step_info, sizeof(step_info), "[%s %d/%d]  ", step_name, step_num, total_steps);
    } else {
        snprintf(step_info, sizeof(step_info), "[%s]  ", step_name);
    }
    int spaces = g_term_cols - (int)strlen(title) - (int)strlen(step_info);
    for (int i = 0; i < spaces; i++) tui_buf_write(" ");
    tui_buf_write(step_info);

    /* Bottom keybind helper bar */
    tui_cursor(g_term_rows, 1);
    tui_buf_write("\033[0;30;47m");
    const char *help = " <Up/Down> Navigate | <Tab> Switch Field | <Space> Toggle | <Enter> Select ";
    tui_buf_write(help);
    int rem = g_term_cols - (int)strlen(help);
    for (int i = 0; i < rem; i++) tui_buf_write(" ");
    tui_buf_write("\033[0m");
}

/* Draw a styled dialog box with drop shadow */
static void tui_draw_box(int y, int x, int h, int w, const char *title, bool danger) {
    /* Drop shadow on right and bottom */
    for (int r = y + 1; r <= y + h; r++) {
        tui_cursor(r, x + w);
        tui_buf_write("\033[40m  \033[0m");
    }
    tui_cursor(y + h, x + 2);
    tui_buf_write("\033[40m");
    for (int c = 0; c < w; c++) tui_buf_write(" ");
    tui_buf_write("\033[0m");

    /* Box content background: Gray / White */
    const char *bg_col = danger ? "\033[41;37m" : "\033[47;30m";
    const char *border_col = danger ? "\033[1;37;41m" : "\033[1;30;47m";

    /* Top border */
    tui_cursor(y, x);
    tui_buf_write(border_col);
    tui_buf_write("+");
    int title_len = (int)strlen(title);
    int dash_total = w - 2;
    int dash_left = (dash_total - (title_len > 0 ? title_len + 2 : 0)) / 2;
    int dash_right = dash_total - dash_left - (title_len > 0 ? title_len + 2 : 0);

    for (int i = 0; i < dash_left; i++) tui_buf_write("-");
    if (title_len > 0) {
        tui_buf_printf(" %s ", title);
    }
    for (int i = 0; i < dash_right; i++) tui_buf_write("-");
    tui_buf_write("+");

    /* Inner rows */
    for (int r = y + 1; r < y + h - 1; r++) {
        tui_cursor(r, x);
        tui_buf_write(border_col);
        tui_buf_write("|");
        tui_buf_write(bg_col);
        for (int c = 0; c < w - 2; c++) tui_buf_write(" ");
        tui_buf_write(border_col);
        tui_buf_write("|");
    }

    /* Bottom border */
    tui_cursor(y + h - 1, x);
    tui_buf_write(border_col);
    tui_buf_write("+");
    for (int c = 0; c < w - 2; c++) tui_buf_write("-");
    tui_buf_write("+");
    tui_buf_write("\033[0m");
}

/* Draw a standard button */
static void tui_draw_button(int y, int x, const char *label, bool active) {
    tui_cursor(y, x);
    if (active) {
        tui_buf_printf("\033[1;37;44m< %s >\033[0m", label);
    } else {
        tui_buf_printf("\033[0;30;47m[ %s ]\033[0m", label);
    }
}

/* Draw an input text field */
static void tui_draw_input(int y, int x, int w, const char *val, bool active, bool mask) {
    tui_cursor(y, x);
    if (active) {
        tui_buf_write("\033[1;37;44m");
    } else {
        tui_buf_write("\033[0;30;47m");
    }

    tui_buf_write("[ ");
    int val_len = (int)strlen(val);
    int avail = w - 4;
    for (int i = 0; i < avail; i++) {
        if (i < val_len) {
            tui_buf_write(mask ? "*" : (char[]){val[i], 0});
        } else {
            tui_buf_write(active && i == val_len ? "_" : " ");
        }
    }
    tui_buf_write(" ]\033[0m");
}

/* Check if device name represents a whole disk (not a partition) */
static bool is_whole_disk(const char *name) {
    if (!name || !*name) return false;

    /* Filter non-disk devices */
    if (strncmp(name, "ram", 3) == 0 ||
        strncmp(name, "loop", 4) == 0 ||
        strncmp(name, "sr", 2) == 0 ||
        strncmp(name, "cdrom", 5) == 0 ||
        strncmp(name, "zram", 4) == 0 ||
        strncmp(name, "dm-", 3) == 0 ||
        strncmp(name, "md", 2) == 0) {
        return false;
    }

    /* NVMe: nvmeXnY (e.g. nvme0n1, nvme1n1).
     * Partitions have 'p' followed by digits (e.g. nvme0n1p1). */
    if (strncmp(name, "nvme", 4) == 0) {
        const char *p = name + 4;
        if (!isdigit(*p)) return false;
        while (isdigit(*p)) p++;
        if (*p != 'n') return false;
        p++;
        if (!isdigit(*p)) return false;
        while (isdigit(*p)) p++;
        /* Must end directly after the namespace number (no trailing 'p') */
        return (*p == '\0');
    }

    /* SATA (AvoryOS AHCI driver): sata0, sata1...
     * Partitions are sata01, sata02, sata0p1... (more than 1 digit or 'p') */
    if (strncmp(name, "sata", 4) == 0) {
        const char *p = name + 4;
        if (!isdigit(*p)) return false;
        p++;
        /* Whole disk has exactly one digit */
        return (*p == '\0');
    }

    /* ATA (AvoryOS IDE/ATA driver): ata0, ata1...
     * Partitions are ata01, ata02, ata0p1... */
    if (strncmp(name, "ata", 3) == 0) {
        const char *p = name + 3;
        if (!isdigit(*p)) return false;
        p++;
        /* Whole disk has exactly one digit */
        return (*p == '\0');
    }

    /* SCSI / VirtIO / IDE: sda, sdb, vda, vdb, hda, hdb...
     * Partitions have trailing digits: sda1, sda2, vda1... */
    if (strncmp(name, "sd", 2) == 0 ||
        strncmp(name, "vd", 2) == 0 ||
        strncmp(name, "hd", 2) == 0) {
        const char *p = name + 2;
        if (!isalpha(*p)) return false;
        while (isalpha(*p)) p++;
        return (*p == '\0');
    }

    return false;
}

static void add_disk_if_new(const char *name, uint64_t size_bytes) {
    if (!name || !*name) return;
    if (!is_whole_disk(name)) return;
    if (size_bytes < 104857600ULL) return; /* Skip < 100MB */

    for (int i = 0; i < g_disk_count; i++) {
        if (strcmp(g_disks[i].name, name) == 0) return;
    }
    if (g_disk_count >= MAX_DISKS) return;

    disk_t *d = &g_disks[g_disk_count];
    memset(d, 0, sizeof(*d));
    strncpy(d->name, name, sizeof(d->name) - 1);
    snprintf(d->dev_path, sizeof(d->dev_path), "/dev/%s", name);
    d->size_bytes = size_bytes;

    double mb = (double)d->size_bytes / (1024.0 * 1024.0);
    if (mb >= 1024.0) {
        snprintf(d->size_str, sizeof(d->size_str), "%.1f GB", mb / 1024.0);
    } else {
        snprintf(d->size_str, sizeof(d->size_str), "%.0f MB", mb);
    }

    if (strncmp(name, "nvme", 4) == 0) strcpy(d->model, "NVMe Solid State Drive");
    else if (strncmp(name, "sata", 4) == 0) strcpy(d->model, "AHCI / SATA Storage Drive");
    else if (strncmp(name, "ata", 3) == 0) strcpy(d->model, "ATA / IDE Storage Drive");
    else if (strncmp(name, "vd", 2) == 0) strcpy(d->model, "VirtIO Block Device");
    else strcpy(d->model, "SCSI / Direct Disk Drive");

    g_disk_count++;
}

/* ──────────────────────────────────────────────────────────────────────────
 * Hardware & Disk Detection
 * ────────────────────────────────────────────────────────────────────────── */
static void detect_disks(void) {
    g_disk_count = 0;

    /* 1. Primary: read /proc/partitions (kernel's canonical block device registry) */
    FILE *pp = fopen("/proc/partitions", "r");
    if (pp) {
        char line[256];
        while (fgets(line, sizeof(line), pp) && g_disk_count < MAX_DISKS) {
            unsigned int maj = 0, min = 0;
            unsigned long long blocks = 0;
            char name[64];
            memset(name, 0, sizeof(name));
            if (sscanf(line, "%u %u %llu %63s", &maj, &min, &blocks, name) == 4) {
                add_disk_if_new(name, blocks * 1024ULL);
            }
        }
        fclose(pp);
    }

    /* 2. Direct probe of /dev block device nodes via ioctl */
    DIR *devdir = opendir("/dev");
    if (devdir) {
        struct dirent *ent;
        while ((ent = readdir(devdir)) != NULL && g_disk_count < MAX_DISKS) {
            if (ent->d_name[0] == '.') continue;
            const char *name = ent->d_name;
            if (!is_whole_disk(name)) continue;

            char devpath[128];
            snprintf(devpath, sizeof(devpath), "/dev/%s", name);
            int fd = open(devpath, O_RDONLY);
            if (fd >= 0) {
                uint64_t bytes = 0;
                if (ioctl(fd, 0x80081272, &bytes) == 0 && bytes > 0) { // BLKGETSIZE64
                    add_disk_if_new(name, bytes);
                } else {
                    unsigned long sectors = 0;
                    if (ioctl(fd, 0x1260, &sectors) == 0 && sectors > 0) { // BLKGETSIZE
                        add_disk_if_new(name, (uint64_t)sectors * 512ULL);
                    }
                }
                close(fd);
            }
        }
        closedir(devdir);
    }

    /* 3. Fallback: check /sys/block and /sys/class/block */
    DIR *dir = opendir("/sys/block");
    if (dir) {
        struct dirent *ent;
        while ((ent = readdir(dir)) != NULL && g_disk_count < MAX_DISKS) {
            if (ent->d_name[0] == '.') continue;
            const char *name = ent->d_name;
            if (!is_whole_disk(name)) continue;

            char path[256];
            snprintf(path, sizeof(path), "/sys/block/%s/size", name);
            FILE *f = fopen(path, "r");
            uint64_t sectors = 0;
            if (f) {
                if (fscanf(f, "%llu", (unsigned long long *)&sectors) != 1) sectors = 0;
                fclose(f);
            }
            if (sectors == 0) {
                snprintf(path, sizeof(path), "/sys/class/block/%s/size", name);
                f = fopen(path, "r");
                if (f) {
                    if (fscanf(f, "%llu", (unsigned long long *)&sectors) != 1) sectors = 0;
                    fclose(f);
                }
            }
            if (sectors > 0) {
                add_disk_if_new(name, sectors * 512ULL);
            }
        }
        closedir(dir);
    }

    /* Tag live boot media from /proc/mounts */
    FILE *mounts = fopen("/proc/mounts", "r");
    if (mounts) {
        char line[256];
        while (fgets(line, sizeof(line), mounts)) {
            char dev[64], mnt[64];
            if (sscanf(line, "%63s %63s", dev, mnt) == 2 && strcmp(mnt, "/") == 0) {
                for (int i = 0; i < g_disk_count; i++) {
                    if (strstr(dev, g_disks[i].name) != NULL) {
                        g_disks[i].is_live_media = true;
                    }
                }
            }
        }
        fclose(mounts);
    }
}

/* ──────────────────────────────────────────────────────────────────────────
 * Step 1: Welcome Screen
 * ────────────────────────────────────────────────────────────────────────── */
static wizard_step_t screen_welcome(void) {
    int active_btn = 0; /* 0: Install, 1: Shell, 2: Reboot */
    int box_w = 68;
    int box_h = 19;

    while (1) {
        int box_x = (g_term_cols - box_w) / 2;
        int box_y = (g_term_rows - box_h) / 2;

        tui_draw_backdrop("Welcome", 1, 7);
        tui_draw_box(box_y, box_x, box_h, box_w, "Welcome to AvoryOS", false);

        /* Content */
        tui_cursor(box_y + 2, box_x + 6);
        tui_buf_write("\033[1;34;47m   ___                                ____  _____ \033[0m");
        tui_cursor(box_y + 3, box_x + 6);
        tui_buf_write("\033[1;34;47m  / _ \\ _  _____  ______ _____ _____ / __ \\/ ___/ \033[0m");
        tui_cursor(box_y + 4, box_x + 6);
        tui_buf_write("\033[1;34;47m / /_\\ | |/ / _ \\/ __/ // / _ \\/ __// /_/ /\\__ \\  \033[0m");
        tui_cursor(box_y + 5, box_x + 6);
        tui_buf_write("\033[1;34;47m/_/   \\_|___/\\___/_/  \\_, /\\___/_/  \\____//____/  \033[0m");
        tui_cursor(box_y + 6, box_x + 6);
        tui_buf_write("\033[1;34;47m                      /___/                       \033[0m");

        tui_cursor(box_y + 8, box_x + 4);
        tui_buf_write("\033[0;30;47mWelcome to the AvoryOS TTY Installation Suite.\033[0m");
        tui_cursor(box_y + 10, box_x + 4);
        tui_buf_write("\033[0;30;47mThis wizard will guide you through partitioning your drive,\033[0m");
        tui_cursor(box_y + 11, box_x + 4);
        tui_buf_write("\033[0;30;47minstalling the kernel and base libraries, setting up users,\033[0m");
        tui_cursor(box_y + 12, box_x + 4);
        tui_buf_write("\033[0;30;47mand installing the Limine UEFI/BIOS bootloader.\033[0m");

        /* Buttons */
        int btn_y = box_y + box_h - 3;
        tui_draw_button(btn_y, box_x + 8, "Install", active_btn == 0);
        tui_draw_button(btn_y, box_x + 28, "Shell", active_btn == 1);
        tui_draw_button(btn_y, box_x + 46, "Reboot", active_btn == 2);

        tui_flush();

        int k = tui_read_key();
        if (k == KEY_LEFT || k == KEY_BACKTAB) {
            active_btn = (active_btn + 2) % 3;
        } else if (k == KEY_RIGHT || k == KEY_TAB) {
            active_btn = (active_btn + 1) % 3;
        } else if (k == KEY_ENTER || k == KEY_SPACE) {
            if (active_btn == 0) return STEP_KEYMAP;
            if (active_btn == 1) {
                tui_restore_terminal();
                printf("\n[*] Dropping to Live Shell. Type 'exit' to return to installer.\n");
                system("/bin/bash || /bin/sh");
                tui_init_terminal();
            }
            if (active_btn == 2) {
                tui_restore_terminal();
                sync();
                system("reboot");
                exit(0);
            }
        }
    }
}

/* ──────────────────────────────────────────────────────────────────────────
 * Step 2: Keyboard Layout
 * ────────────────────────────────────────────────────────────────────────── */
static const char *k_keymaps[][2] = {
    {"us", "United States (Standard QWERTY)"},
    {"uk", "United Kingdom (UK Extended)"},
    {"de", "Germany (QWERTZ)"},
    {"fr", "France (AZERTY)"},
    {"es", "Spain"},
    {"it", "Italy"},
    {"se", "Sweden"},
    {"no", "Norway"},
    {"tr", "Turkey"}
};
#define KEYMAP_COUNT (int)(sizeof(k_keymaps) / sizeof(k_keymaps[0]))

static wizard_step_t screen_keymap(void) {
    int sel_idx = 0;
    int focus_btn = 0; /* 0: list, 1: Continue, 2: Back */
    int box_w = 66;
    int box_h = 18;

    while (1) {
        int box_x = (g_term_cols - box_w) / 2;
        int box_y = (g_term_rows - box_h) / 2;

        tui_draw_backdrop("Keymap", 2, 7);
        tui_draw_box(box_y, box_x, box_h, box_w, "Keyboard Layout Selection", false);

        tui_cursor(box_y + 2, box_x + 4);
        tui_buf_write("\033[0;30;47mSelect your keyboard map for the text console:\033[0m");

        /* Keymap list */
        for (int i = 0; i < KEYMAP_COUNT; i++) {
            tui_cursor(box_y + 4 + i, box_x + 6);
            bool is_sel = (i == sel_idx);
            if (is_sel && focus_btn == 0) {
                tui_buf_printf("\033[1;37;44m > [x] %-6s  %-36s\033[0m", k_keymaps[i][0], k_keymaps[i][1]);
            } else if (is_sel) {
                tui_buf_printf("\033[0;34;47m   [*] %-6s  %-36s\033[0m", k_keymaps[i][0], k_keymaps[i][1]);
            } else {
                tui_buf_printf("\033[0;30;47m   [ ] %-6s  %-36s\033[0m", k_keymaps[i][0], k_keymaps[i][1]);
            }
        }

        /* Buttons */
        int btn_y = box_y + box_h - 3;
        tui_draw_button(btn_y, box_x + 14, "Continue", focus_btn == 1);
        tui_draw_button(btn_y, box_x + 36, "Back", focus_btn == 2);

        tui_flush();

        int k = tui_read_key();
        if (focus_btn == 0) {
            if (k == KEY_UP) {
                if (sel_idx > 0) sel_idx--;
            } else if (k == KEY_DOWN) {
                if (sel_idx < KEYMAP_COUNT - 1) sel_idx++;
            } else if (k == KEY_TAB) {
                focus_btn = 1;
            } else if (k == KEY_ENTER || k == KEY_SPACE) {
                strncpy(g_keymap, k_keymaps[sel_idx][0], sizeof(g_keymap) - 1);
                focus_btn = 1;
            }
        } else {
            if (k == KEY_LEFT) {
                focus_btn = (focus_btn == 1) ? 2 : 1;
            } else if (k == KEY_RIGHT) {
                focus_btn = (focus_btn == 1) ? 2 : 1;
            } else if (k == KEY_TAB) {
                focus_btn = (focus_btn == 1) ? 2 : 0;
            } else if (k == KEY_BACKTAB) {
                focus_btn = (focus_btn == 2) ? 1 : 0;
            } else if (k == KEY_UP) {
                focus_btn = 0;
            } else if (k == KEY_ENTER) {
                if (focus_btn == 1) {
                    strncpy(g_keymap, k_keymaps[sel_idx][0], sizeof(g_keymap) - 1);
                    return STEP_NETWORK;
                }
                if (focus_btn == 2) return STEP_WELCOME;
            }
        }
    }
}

/* ──────────────────────────────────────────────────────────────────────────
 * Step 3: Network Configuration (Ubuntu Server style)
 * ────────────────────────────────────────────────────────────────────────── */
static wizard_step_t screen_network(void) {
    int choice = g_enable_dhcp ? 0 : 1;
    int focus_btn = 0; /* 0: choice, 1: Continue, 2: Back */
    int box_w = 64;
    int box_h = 16;

    while (1) {
        int box_x = (g_term_cols - box_w) / 2;
        int box_y = (g_term_rows - box_h) / 2;

        tui_draw_backdrop("Network", 3, 7);
        tui_draw_box(box_y, box_x, box_h, box_w, "Network Configuration", false);

        tui_cursor(box_y + 2, box_x + 4);
        tui_buf_write("\033[0;30;47mConfigure primary network adapter (eth0 / rtl8139):\033[0m");

        /* Options */
        tui_cursor(box_y + 5, box_x + 6);
        if (choice == 0 && focus_btn == 0) {
            tui_buf_write("\033[1;37;44m (*) Automatic DHCP (Recommended)               \033[0m");
        } else if (choice == 0) {
            tui_buf_write("\033[0;34;47m (*) Automatic DHCP (Recommended)               \033[0m");
        } else {
            tui_buf_write("\033[0;30;47m ( ) Automatic DHCP (Recommended)               \033[0m");
        }

        tui_cursor(box_y + 7, box_x + 6);
        if (choice == 1 && focus_btn == 0) {
            tui_buf_write("\033[1;37;44m ( ) Skip network configuration for now         \033[0m");
        } else if (choice == 1) {
            tui_buf_write("\033[0;34;47m (*) Skip network configuration for now         \033[0m");
        } else {
            tui_buf_write("\033[0;30;47m ( ) Skip network configuration for now         \033[0m");
        }

        /* Buttons */
        int btn_y = box_y + box_h - 3;
        tui_draw_button(btn_y, box_x + 14, "Continue", focus_btn == 1);
        tui_draw_button(btn_y, box_x + 36, "Back", focus_btn == 2);

        tui_flush();

        int k = tui_read_key();
        if (focus_btn == 0) {
            if (k == KEY_UP || k == KEY_DOWN) {
                choice = 1 - choice;
            } else if (k == KEY_TAB) {
                focus_btn = 1;
            } else if (k == KEY_ENTER || k == KEY_SPACE) {
                g_enable_dhcp = (choice == 0);
                focus_btn = 1;
            }
        } else {
            if (k == KEY_LEFT || k == KEY_RIGHT) {
                focus_btn = (focus_btn == 1) ? 2 : 1;
            } else if (k == KEY_TAB) {
                focus_btn = (focus_btn == 1) ? 2 : 0;
            } else if (k == KEY_UP) {
                focus_btn = 0;
            } else if (k == KEY_ENTER) {
                if (focus_btn == 1) {
                    g_enable_dhcp = (choice == 0);
                    return STEP_DISK_SELECT;
                }
                if (focus_btn == 2) return STEP_KEYMAP;
            }
        }
    }
}

/* ──────────────────────────────────────────────────────────────────────────
 * Step 4: Storage & Disk Selection
 * ────────────────────────────────────────────────────────────────────────── */
static wizard_step_t screen_disk_select(void) {
    detect_disks();
    if (g_disk_count > 1 && g_disks[0].is_live_media && g_selected_disk_idx == 0) {
        for (int i = 0; i < g_disk_count; i++) {
            if (!g_disks[i].is_live_media) {
                g_selected_disk_idx = i;
                break;
            }
        }
    }
    int focus_btn = 0; /* 0: disks, 1: Continue, 2: Back */
    int box_w = 72;
    int box_h = 18;

    if (g_disk_count == 0) {
        /* No disks found alert */
        while (1) {
            int box_x = (g_term_cols - box_w) / 2;
            int box_y = (g_term_rows - box_h) / 2;
            tui_draw_backdrop("Storage", 4, 7);
            tui_draw_box(box_y, box_x, box_h, box_w, "Storage Error", true);
            tui_cursor(box_y + 4, box_x + 4);
            tui_buf_write("\033[1;37;41mNo storage disks detected!\033[0m");
            tui_cursor(box_y + 6, box_x + 4);
            tui_buf_write("\033[0;37;41mCheck VM settings or controller driver (NVMe/AHCI).\033[0m");
            tui_draw_button(box_y + box_h - 3, box_x + 28, "Back", true);
            tui_flush();
            int k = tui_read_key();
            if (k == KEY_ENTER) return STEP_NETWORK;
        }
    }

    while (1) {
        int box_x = (g_term_cols - box_w) / 2;
        int box_y = (g_term_rows - box_h) / 2;

        tui_draw_backdrop("Storage", 4, 7);
        tui_draw_box(box_y, box_x, box_h, box_w, "Target Storage Device", false);

        tui_cursor(box_y + 2, box_x + 4);
        tui_buf_write("\033[0;30;47mSelect target disk for Guided GPT Partitioning:\033[0m");

        /* Disk table header */
        tui_cursor(box_y + 4, box_x + 4);
        tui_buf_write("\033[1;30;47m  Device     Size       Model / Bus\033[0m");
        tui_cursor(box_y + 5, box_x + 4);
        tui_buf_write("\033[0;30;47m  ------------------------------------------------------------\033[0m");

        /* Disk rows */
        for (int i = 0; i < g_disk_count; i++) {
            tui_cursor(box_y + 6 + i, box_x + 4);
            disk_t *d = &g_disks[i];
            bool is_sel = (i == g_selected_disk_idx);

            char live_tag[32] = "";
            if (d->is_live_media) strcpy(live_tag, " (LIVE USB)");

            if (is_sel && focus_btn == 0) {
                tui_buf_printf("\033[1;37;44m> [x] %-8s %-10s %-28s%s \033[0m",
                               d->name, d->size_str, d->model, live_tag);
            } else if (is_sel) {
                tui_buf_printf("\033[0;34;47m  [*] %-8s %-10s %-28s%s \033[0m",
                               d->name, d->size_str, d->model, live_tag);
            } else {
                tui_buf_printf("\033[0;30;47m  [ ] %-8s %-10s %-28s%s \033[0m",
                               d->name, d->size_str, d->model, live_tag);
            }
        }

        /* Buttons */
        int btn_y = box_y + box_h - 3;
        tui_draw_button(btn_y, box_x + 18, "Continue", focus_btn == 1);
        tui_draw_button(btn_y, box_x + 40, "Back", focus_btn == 2);

        tui_flush();

        int k = tui_read_key();
        if (focus_btn == 0) {
            if (k == KEY_UP) {
                if (g_selected_disk_idx > 0) g_selected_disk_idx--;
            } else if (k == KEY_DOWN) {
                if (g_selected_disk_idx < g_disk_count - 1) g_selected_disk_idx++;
            } else if (k == KEY_TAB) {
                focus_btn = 1;
            } else if (k == KEY_ENTER || k == KEY_SPACE) {
                focus_btn = 1;
            }
        } else {
            if (k == KEY_LEFT || k == KEY_RIGHT) {
                focus_btn = (focus_btn == 1) ? 2 : 1;
            } else if (k == KEY_TAB) {
                focus_btn = (focus_btn == 1) ? 2 : 0;
            } else if (k == KEY_UP) {
                focus_btn = 0;
            } else if (k == KEY_ENTER) {
                if (focus_btn == 1) {
                    if (g_disks[g_selected_disk_idx].is_live_media) {
                        int em_w = 66, em_h = 13;
                        int em_x = (g_term_cols - em_w) / 2;
                        int em_y = (g_term_rows - em_h) / 2;
                        tui_draw_backdrop("Storage", 4, 7);
                        tui_draw_box(em_y, em_x, em_h, em_w, "Cannot Overwrite Live Media", true);
                        tui_cursor(em_y + 3, em_x + 4);
                        tui_buf_printf("\033[1;37;41mTarget disk /dev/%s is currently mounted as root (/)!\033[0m", g_disks[g_selected_disk_idx].name);
                        tui_cursor(em_y + 5, em_x + 4);
                        tui_buf_write("\033[0;37;41mYou cannot erase the active running system drive.\033[0m");
                        tui_cursor(em_y + 6, em_x + 4);
                        tui_buf_write("\033[0;37;41mPlease select a non-root destination storage drive.\033[0m");
                        tui_draw_button(em_y + em_h - 3, em_x + 28, "OK", true);
                        tui_flush();
                        while (tui_read_key() != KEY_ENTER) {}
                        continue;
                    }
                    return STEP_CONFIRM_DISK;
                }
                if (focus_btn == 2) return STEP_NETWORK;
            }
        }
    }
}

/* ──────────────────────────────────────────────────────────────────────────
 * Step 5: Confirmation of Destructive Disk Changes
 * ────────────────────────────────────────────────────────────────────────── */
static wizard_step_t screen_confirm_disk(void) {
    int active_btn = 1; /* 0: Yes Erase, 1: No Cancel (Default safe) */
    int box_w = 68;
    int box_h = 17;
    disk_t *d = &g_disks[g_selected_disk_idx];

    while (1) {
        int box_x = (g_term_cols - box_w) / 2;
        int box_y = (g_term_rows - box_h) / 2;

        tui_draw_backdrop("Confirm Partitioning", 5, 7);
        tui_draw_box(box_y, box_x, box_h, box_w, "Destructive Action Warning", true);

        tui_cursor(box_y + 2, box_x + 4);
        tui_buf_write("\033[1;37;41mWARNING: ALL DATA ON THE TARGET DISK WILL BE DESTROYED!\033[0m");

        tui_cursor(box_y + 4, box_x + 4);
        tui_buf_printf("\033[0;37;41mTarget Device: %s  (%s, %s)\033[0m", d->dev_path, d->size_str, d->model);

        tui_cursor(box_y + 6, box_x + 4);
        tui_buf_write("\033[0;37;41mThe installer will initialize a new GPT partition table:\033[0m");
        tui_cursor(box_y + 7, box_x + 6);
        tui_buf_write("\033[0;37;41m1. Partition 1: 128 MB EFI System Partition (FAT32)\033[0m");
        tui_cursor(box_y + 8, box_x + 6);
        tui_buf_write("\033[0;37;41m2. Partition 2: Remainder AvoryOS Root Filesystem (ext4)\033[0m");
        tui_cursor(box_y + 9, box_x + 6);
        tui_buf_write("\033[0;37;41m3. Bootloader:  Limine (UEFI x86_64 + BIOS MBR)\033[0m");

        tui_cursor(box_y + 11, box_x + 4);
        tui_buf_write("\033[1;37;41mAre you sure you want to write these changes to disk?\033[0m");

        /* Buttons */
        int btn_y = box_y + box_h - 3;
        tui_draw_button(btn_y, box_x + 10, "Yes - Erase and Install", active_btn == 0);
        tui_draw_button(btn_y, box_x + 44, "Cancel / Go Back", active_btn == 1);

        tui_flush();

        int k = tui_read_key();
        if (k == KEY_LEFT || k == KEY_RIGHT || k == KEY_TAB) {
            active_btn = 1 - active_btn;
        } else if (k == KEY_ENTER) {
            if (active_btn == 0) return STEP_COMPONENTS;
            return STEP_DISK_SELECT;
        }
    }
}

/* ──────────────────────────────────────────────────────────────────────────
 * Step 6: Software Components & Desktop Selection
 * ────────────────────────────────────────────────────────────────────────── */
static wizard_step_t screen_components(void) {
    int item_idx = 0; /* 0: Desktop env, 1: Dev tools, 2: Games/media, 3: DE choice, 4: Continue, 5: Back */
    int box_w = 68;
    int box_h = 19;

    while (1) {
        int box_x = (g_term_cols - box_w) / 2;
        int box_y = (g_term_rows - box_h) / 2;

        tui_draw_backdrop("Components", 6, 7);
        tui_draw_box(box_y, box_x, box_h, box_w, "Software & Desktop Environment", false);

        tui_cursor(box_y + 2, box_x + 4);
        tui_buf_write("\033[0;30;47mSelect distribution packages to install:\033[0m");

        /* Base always included */
        tui_cursor(box_y + 4, box_x + 6);
        tui_buf_write("\033[0;32;47m[*] Base System & Kernel (Always Installed)\033[0m");

        /* Desktop Environment */
        tui_cursor(box_y + 5, box_x + 6);
        if (item_idx == 0) {
            tui_buf_printf("\033[1;37;44m[%c] Graphical Desktop Suite (XFCE, KDE, IceWM)\033[0m", g_opt_desktop ? '*' : ' ');
        } else {
            tui_buf_printf("\033[0;30;47m[%c] Graphical Desktop Suite (XFCE, KDE, IceWM)\033[0m", g_opt_desktop ? '*' : ' ');
        }

        /* Dev tools */
        tui_cursor(box_y + 6, box_x + 6);
        if (item_idx == 1) {
            tui_buf_printf("\033[1;37;44m[%c] Development Toolchains (GCC, Make, TCC)\033[0m", g_opt_dev ? '*' : ' ');
        } else {
            tui_buf_printf("\033[0;30;47m[%c] Development Toolchains (GCC, Make, TCC)\033[0m", g_opt_dev ? '*' : ' ');
        }

        /* Games & Media */
        tui_cursor(box_y + 7, box_x + 6);
        if (item_idx == 2) {
            tui_buf_printf("\033[1;37;44m[%c] Ported Games & Media (DOOM, Quake II, Firefox)\033[0m", g_opt_games ? '*' : ' ');
        } else {
            tui_buf_printf("\033[0;30;47m[%c] Ported Games & Media (DOOM, Quake II, Firefox)\033[0m", g_opt_games ? '*' : ' ');
        }

        /* Desktop flavor radio */
        tui_cursor(box_y + 9, box_x + 4);
        tui_buf_write("\033[0;30;47mDefault Session to Launch on Startup:\033[0m");

        const char *de_names[] = {"XFCE 4 (Traditional)", "KDE Plasma 6", "IceWM (Lightweight)", "Text Console"};
        for (int i = 0; i < 4; i++) {
            tui_cursor(box_y + 11 + i, box_x + 6);
            bool is_sel = (g_desktop_choice == i);
            if (item_idx == 3 + i) {
                tui_buf_printf("\033[1;37;44m(%c) %-32s\033[0m", is_sel ? '*' : ' ', de_names[i]);
            } else {
                tui_buf_printf("\033[0;30;47m(%c) %-32s\033[0m", is_sel ? '*' : ' ', de_names[i]);
            }
        }

        /* Buttons */
        int btn_y = box_y + box_h - 3;
        tui_draw_button(btn_y, box_x + 16, "Continue", item_idx == 7);
        tui_draw_button(btn_y, box_x + 38, "Back", item_idx == 8);

        tui_flush();

        int k = tui_read_key();
        if (k == KEY_UP) {
            if (item_idx > 0) item_idx--;
        } else if (k == KEY_DOWN) {
            if (item_idx < 8) item_idx++;
        } else if (k == KEY_TAB) {
            if (item_idx < 7) item_idx = 7;
            else if (item_idx == 7) item_idx = 8;
            else item_idx = 0;
        } else if (k == KEY_SPACE || k == KEY_ENTER) {
            if (item_idx == 0) g_opt_desktop = !g_opt_desktop;
            else if (item_idx == 1) g_opt_dev = !g_opt_dev;
            else if (item_idx == 2) g_opt_games = !g_opt_games;
            else if (item_idx >= 3 && item_idx <= 6) g_desktop_choice = item_idx - 3;
            else if (item_idx == 7) return STEP_IDENTITY;
            else if (item_idx == 8) return STEP_CONFIRM_DISK;
        }
    }
}

/* ──────────────────────────────────────────────────────────────────────────
 * Step 7: Identity & User Accounts (Ubuntu Server style)
 * ────────────────────────────────────────────────────────────────────────── */
static wizard_step_t screen_identity(void) {
    int active_field = 0; /* 0: Name, 1: Hostname, 2: Username, 3: Pass, 4: PassConfirm, 5: Cont, 6: Back */
    int box_w = 66;
    int box_h = 18;

    while (1) {
        int box_x = (g_term_cols - box_w) / 2;
        int box_y = (g_term_rows - box_h) / 2;

        tui_draw_backdrop("Identity", 7, 7);
        tui_draw_box(box_y, box_x, box_h, box_w, "System Profile & Accounts", false);

        tui_cursor(box_y + 2, box_x + 4);
        tui_buf_write("\033[0;30;47mConfigure your hostname and administrative account:\033[0m");

        /* Labels and input fields */
        tui_cursor(box_y + 4, box_x + 4);
        tui_buf_write("\033[0;30;47mYour name:     \033[0m");
        tui_draw_input(box_y + 4, box_x + 20, 38, g_fullname, active_field == 0, false);

        tui_cursor(box_y + 6, box_x + 4);
        tui_buf_write("\033[0;30;47mHostname:      \033[0m");
        tui_draw_input(box_y + 6, box_x + 20, 38, g_hostname, active_field == 1, false);

        tui_cursor(box_y + 8, box_x + 4);
        tui_buf_write("\033[0;30;47mUsername:      \033[0m");
        tui_draw_input(box_y + 8, box_x + 20, 38, g_username, active_field == 2, false);

        tui_cursor(box_y + 10, box_x + 4);
        tui_buf_write("\033[0;30;47mPassword:      \033[0m");
        tui_draw_input(box_y + 10, box_x + 20, 38, g_password, active_field == 3, true);

        tui_cursor(box_y + 12, box_x + 4);
        tui_buf_write("\033[0;30;47mConfirm pass:  \033[0m");
        tui_draw_input(box_y + 12, box_x + 20, 38, g_password_confirm, active_field == 4, true);

        /* Buttons */
        int btn_y = box_y + box_h - 3;
        tui_draw_button(btn_y, box_x + 16, "Continue", active_field == 5);
        tui_draw_button(btn_y, box_x + 38, "Back", active_field == 6);

        tui_flush();

        int k = tui_read_key();
        if (k == KEY_TAB || k == KEY_DOWN) {
            active_field = (active_field + 1) % 7;
        } else if (k == KEY_BACKTAB || k == KEY_UP) {
            active_field = (active_field + 6) % 7;
        } else if (active_field == 5 && k == KEY_ENTER) {
            /* Validation */
            if (strcmp(g_password, g_password_confirm) != 0) {
                /* Show mismatch notification */
                continue;
            }
            return STEP_INSTALLING;
        } else if (active_field == 6 && k == KEY_ENTER) {
            return STEP_COMPONENTS;
        } else if (active_field <= 4) {
            char *target = NULL;
            size_t maxlen = 31;
            if (active_field == 0) { target = g_fullname; maxlen = 63; }
            else if (active_field == 1) { target = g_hostname; maxlen = 31; }
            else if (active_field == 2) { target = g_username; maxlen = 31; }
            else if (active_field == 3) { target = g_password; maxlen = 63; }
            else if (active_field == 4) { target = g_password_confirm; maxlen = 63; }

            if (k == KEY_BACKSPACE) {
                size_t len = strlen(target);
                if (len > 0) target[len - 1] = '\0';
            } else if (k >= 32 && k <= 126) {
                size_t len = strlen(target);
                if (len < maxlen) {
                    target[len] = (char)k;
                    target[len + 1] = '\0';
                }
            } else if (k == KEY_ENTER) {
                active_field++;
            }
        }
    }
}

/* ──────────────────────────────────────────────────────────────────────────
 * Step 8: Live Progress & Installation Execution
 * ────────────────────────────────────────────────────────────────────────── */
static void draw_progress_ui(int pct, const char *status) {
    int box_w = 68;
    int box_h = 14;
    int box_x = (g_term_cols - box_w) / 2;
    int box_y = (g_term_rows - box_h) / 2;

    tui_draw_backdrop("Installing", 7, 7);
    tui_draw_box(box_y, box_x, box_h, box_w, "Installing AvoryOS", false);

    tui_cursor(box_y + 3, box_x + 6);
    tui_buf_printf("\033[0;30;47mStatus: %-52s\033[0m", status);

    /* Progress bar */
    tui_cursor(box_y + 6, box_x + 6);
    tui_buf_write("\033[1;30;47m[ ");
    int bar_width = box_w - 20;
    int filled = (bar_width * pct) / 100;
    tui_buf_write("\033[1;37;44m");
    for (int i = 0; i < filled; i++) tui_buf_write("#");
    tui_buf_write("\033[0;30;47m");
    for (int i = filled; i < bar_width; i++) tui_buf_write("-");
    tui_buf_printf(" ] %3d%%\033[0m", pct);

    tui_cursor(box_y + 9, box_x + 6);
    tui_buf_write("\033[0;34;47mPlease wait while system files and bootloader are installed...\033[0m");

    tui_flush();
}

static int installer_exec(const char *cmd) {
    pid_t pid = fork();
    if (pid == 0) {
        int devnull = open("/dev/null", O_RDWR);
        if (devnull >= 0) {
            dup2(devnull, STDOUT_FILENO);
            dup2(devnull, STDERR_FILENO);
            close(devnull);
        }
        /* Prefer busybox sh to avoid glibc bash locale fnmatch crashes */
        if (access("/bin/busybox", X_OK) == 0) {
            execl("/bin/busybox", "sh", "-c", cmd, (char *)NULL);
        }
        execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
        _exit(127);
    }
    if (pid < 0) return -1;
    int status = 0;
    waitpid(pid, &status, 0);
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

static int ensure_dir(const char *path) {
    char tmp[512];
    snprintf(tmp, sizeof(tmp), "%s", path);
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            mkdir(tmp, 0755);
            *p = '/';
        }
    }
    return mkdir(tmp, 0755);
}

static wizard_step_t screen_installing(void) {
    disk_t *d = &g_disks[g_selected_disk_idx];
    char cmd[1024];

    /* Unmount any previous test mounts first */
    umount2(TARGET_MOUNT "/boot/efi", 2);
    umount2(TARGET_MOUNT, 2);

    /* Ensure clean base disk name without any partition index */
    char base_disk[32];
    strncpy(base_disk, d->name, sizeof(base_disk) - 1);
    base_disk[sizeof(base_disk) - 1] = '\0';
    char *p_idx = strchr(base_disk, 'p');
    if (strncmp(base_disk, "nvme", 4) == 0 && p_idx && isdigit(p_idx[1])) {
        *p_idx = '\0';
    }
    char base_dev[64];
    snprintf(base_dev, sizeof(base_dev), "/dev/%s", base_disk);

    char esp_dev[64], root_dev[64];
    if (strncmp(base_disk, "nvme", 4) == 0) {
        snprintf(esp_dev, sizeof(esp_dev), "%sp1", base_dev);
        snprintf(root_dev, sizeof(root_dev), "%sp2", base_dev);
    } else {
        snprintf(esp_dev, sizeof(esp_dev), "%s1", base_dev);
        snprintf(root_dev, sizeof(root_dev), "%s2", base_dev);
    }

    /* 1. Partition disk with parted */
    draw_progress_ui(10, "Creating GPT partition table on target disk...");
    sleep(1);

    snprintf(cmd, sizeof(cmd),
             "parted -s %s mklabel gpt && "
             "parted -s %s mkpart 'EFI' fat32 1MiB 129MiB && "
             "parted -s %s set 1 esp on && "
             "parted -s %s mkpart 'AvoryOS-Root' ext4 129MiB 100%%",
             base_dev, base_dev, base_dev, base_dev);
    installer_exec(cmd);

    /* Direct in-kernel partition reread */
    int blk_fd = open(base_dev, O_RDONLY);
    if (blk_fd >= 0) {
        ioctl(blk_fd, 0x125F, 0); /* BLKRRPART */
        close(blk_fd);
    }
    snprintf(cmd, sizeof(cmd), "blockdev --rereadpt %s >/dev/null 2>&1 || true", base_dev);
    installer_exec(cmd);

    /* Wait for partition device nodes to appear */
    for (int retry = 0; retry < 30; retry++) {
        if (access(root_dev, F_OK) == 0 && access(esp_dev, F_OK) == 0) break;
        char alt_root[64], alt_esp[64];
        if (strncmp(base_disk, "sata", 4) == 0 || strncmp(base_disk, "ata", 3) == 0) {
            snprintf(alt_esp, sizeof(alt_esp), "%sp1", base_dev);
            snprintf(alt_root, sizeof(alt_root), "%sp2", base_dev);
            if (access(alt_root, F_OK) == 0 && access(alt_esp, F_OK) == 0) {
                strcpy(root_dev, alt_root);
                strcpy(esp_dev, alt_esp);
                break;
            }
        }
        usleep(150000);
    }

    /* 2. Format ESP */
    draw_progress_ui(25, "Formatting EFI System Partition (FAT32)...");
    snprintf(cmd, sizeof(cmd), "mkfs.vfat -F 32 -n 'EFI' %s", esp_dev);
    installer_exec(cmd);

    /* 3. Format Root */
    draw_progress_ui(35, "Formatting AvoryOS Root partition (ext4)...");
    snprintf(cmd, sizeof(cmd), "mke2fs -t ext4 -F -L 'AvoryOS-Root' %s", root_dev);
    installer_exec(cmd);

    /* 4. Mount target */
    draw_progress_ui(45, "Mounting target filesystem...");
    ensure_dir(TARGET_MOUNT);
    int mnt_res = mount(root_dev, TARGET_MOUNT, "ext4", 0, NULL);
    if (mnt_res != 0) {
        snprintf(cmd, sizeof(cmd), "mount -t ext4 %s " TARGET_MOUNT, root_dev);
        mnt_res = installer_exec(cmd);
    }
    if (mnt_res != 0) {
        int box_w = 66, box_h = 13;
        int box_x = (g_term_cols - box_w) / 2;
        int box_y = (g_term_rows - box_h) / 2;
        tui_draw_backdrop("Mount Error", 7, 7);
        tui_draw_box(box_y, box_x, box_h, box_w, "Mount Failure", true);
        tui_cursor(box_y + 3, box_x + 4);
        tui_buf_printf("\033[1;37;41mFailed to mount root partition: %s\033[0m", root_dev);
        tui_cursor(box_y + 5, box_x + 4);
        tui_buf_write("\033[0;37;41mCould not mount the target partition onto /mnt/target.\033[0m");
        tui_cursor(box_y + 6, box_x + 4);
        tui_buf_write("\033[0;37;41mPlease check disk status or run manual partitioning.\033[0m");
        tui_draw_button(box_y + box_h - 3, box_x + 26, "Back", true);
        tui_flush();
        while (tui_read_key() != KEY_ENTER) {}
        return STEP_DISK_SELECT;
    }

    ensure_dir(TARGET_MOUNT "/boot/efi");
    if (mount(esp_dev, TARGET_MOUNT "/boot/efi", "vfat", 0, NULL) != 0) {
        snprintf(cmd, sizeof(cmd), "mount -t vfat %s " TARGET_MOUNT "/boot/efi", esp_dev);
        installer_exec(cmd);
    }

    /* 5. Copy system files */
    const char *dirs[] = {"bin", "sbin", "lib", "lib64", "usr", "etc", "opt", "var", "root", "home"};
    int num_dirs = sizeof(dirs) / sizeof(dirs[0]);
    for (int i = 0; i < num_dirs; i++) {
        char status_msg[128];
        snprintf(status_msg, sizeof(status_msg), "Copying /%s to target system...", dirs[i]);
        int pct = 50 + (i * 25) / num_dirs;
        draw_progress_ui(pct, status_msg);

        snprintf(cmd, sizeof(cmd), "cp -a /%s " TARGET_MOUNT "/", dirs[i]);
        installer_exec(cmd);
    }

    /* Create virtual mount points */
    ensure_dir(TARGET_MOUNT "/proc");
    ensure_dir(TARGET_MOUNT "/sys");
    ensure_dir(TARGET_MOUNT "/dev");
    ensure_dir(TARGET_MOUNT "/run");
    ensure_dir(TARGET_MOUNT "/tmp");
    ensure_dir(TARGET_MOUNT "/mnt");
    ensure_dir(TARGET_MOUNT "/boot/limine");
    ensure_dir(TARGET_MOUNT "/boot/efi/EFI/BOOT");
    chmod(TARGET_MOUNT "/tmp", 01777);
    chmod(TARGET_MOUNT "/run", 01777);

    /* 6. Install Bootloader (Limine) & Kernel */
    draw_progress_ui(80, "Installing Limine Bootloader and Kernel...");
    ensure_dir(TARGET_MOUNT "/boot/efi/boot");
    ensure_dir(TARGET_MOUNT "/boot/efi/EFI/BOOT");
    if (access("/boot/kernel", F_OK) == 0) {
        installer_exec("cp /boot/kernel " TARGET_MOUNT "/boot/kernel");
        installer_exec("cp /boot/kernel " TARGET_MOUNT "/boot/efi/boot/kernel || true");
    }
    if (access("/usr/share/limine/BOOTX64.EFI", F_OK) == 0) {
        installer_exec("cp /usr/share/limine/BOOTX64.EFI " TARGET_MOUNT "/boot/efi/EFI/BOOT/BOOTX64.EFI");
    }
    if (access("/usr/share/limine/limine-bios.sys", F_OK) == 0) {
        installer_exec("cp /usr/share/limine/limine-bios.sys " TARGET_MOUNT "/boot/limine/");
    }
    if (access("/usr/share/limine/boo.png", F_OK) == 0) {
        installer_exec("cp /usr/share/limine/boo.png " TARGET_MOUNT "/boot/limine/");
        installer_exec("cp /usr/share/limine/boo.png " TARGET_MOUNT "/boot/efi/EFI/BOOT/boo.png || true");
    }

    /* Write Limine config */
    FILE *lconf = fopen(TARGET_MOUNT "/boot/limine/limine.conf", "w");
    if (lconf) {
        fprintf(lconf,
                "timeout: 3\n"
                "interface_resolution: 1280x800\n"
                "wallpaper: boot():/boot/limine/boo.png\n"
                "term_wallpaper: boot():/boot/limine/boo.png\n\n"
                "/AvoryOS\n"
                "    protocol: limine\n"
                "    path: boot():/boot/kernel\n"
                "    cmdline: init=/sbin/init quiet splash\n"
                "    resolution: 1280x800\n\n"
                "/AvoryOS (Rescue Shell)\n"
                "    protocol: limine\n"
                "    path: boot():/boot/kernel\n"
                "    cmdline: init=/bin/bash screenlog\n"
                "    resolution: 1280x800\n");
        fclose(lconf);
        /* Sync limine.conf to ESP as well */
        installer_exec("cp " TARGET_MOUNT "/boot/limine/limine.conf " TARGET_MOUNT "/boot/efi/EFI/BOOT/limine.conf || true");
        installer_exec("cp " TARGET_MOUNT "/boot/limine/limine.conf " TARGET_MOUNT "/boot/efi/limine.conf || true");
    }

    /* Install Limine BIOS boot sector */
    snprintf(cmd, sizeof(cmd), "limine bios-install %s || true", base_dev);
    installer_exec(cmd);

    /* 7. Configuration files: /etc/fstab, /etc/hostname, /etc/hosts */
    draw_progress_ui(90, "Configuring fstab, hostname, and user accounts...");
    FILE *fstab = fopen(TARGET_MOUNT "/etc/fstab", "w");
    if (fstab) {
        fprintf(fstab,
                "# /etc/fstab: static file system information\n"
                "%s    /          ext4   noatime,rw   0 1\n"
                "%s    /boot/efi  vfat   defaults     0 2\n"
                "tmpfs        /tmp       tmpfs  defaults     0 0\n"
                "proc         /proc      proc   defaults     0 0\n"
                "sysfs        /sys       sysfs  defaults     0 0\n",
                root_dev, esp_dev);
        fclose(fstab);
    }

    FILE *hn = fopen(TARGET_MOUNT "/etc/hostname", "w");
    if (hn) {
        fprintf(hn, "%s\n", g_hostname);
        fclose(hn);
    }

    FILE *hosts = fopen(TARGET_MOUNT "/etc/hosts", "w");
    if (hosts) {
        fprintf(hosts,
                "127.0.0.1   localhost %s\n"
                "::1         localhost ip6-localhost ip6-loopback\n",
                g_hostname);
        fclose(hosts);
    }

    /* User account creation */
    if (g_username[0]) {
        snprintf(cmd, sizeof(cmd),
                 "chroot " TARGET_MOUNT " /usr/sbin/useradd -m -s /bin/bash -c '%s' -G wheel,audio,video,input %s || true",
                 g_fullname, g_username);
        installer_exec(cmd);

        if (g_password[0]) {
            snprintf(cmd, sizeof(cmd),
                     "echo '%s:%s' | chpasswd --root " TARGET_MOUNT " || true",
                     g_username, g_password);
            installer_exec(cmd);
        }
    }

    /* Default desktop session */
    const char *xinit_cmd = "exec startxfce4";
    if (g_desktop_choice == 1) xinit_cmd = "exec startplasma-x11";
    else if (g_desktop_choice == 2) xinit_cmd = "exec icewm-session";
    else if (g_desktop_choice == 3) xinit_cmd = "";

    if (xinit_cmd[0]) {
        /* Root's home is "/" on AvoryOS. Join the XDG defaults through
         * "${HOME%/}" so startx sessions never export //.cache-style paths:
         * WebKitGTK rejects those as sandbox roots and Epiphany aborts on the
         * critical that follows (docs/epiphany-pulse-launch-abort.md). */
        const char *xdg_env =
            "_avory_home=\"${HOME:-/}\"\n"
            "_avory_home=\"${_avory_home%/}\"\n"
            "export XDG_CONFIG_HOME=\"${XDG_CONFIG_HOME:-${_avory_home}/.config}\"\n"
            "export XDG_CACHE_HOME=\"${XDG_CACHE_HOME:-${_avory_home}/.cache}\"\n"
            "export XDG_DATA_HOME=\"${XDG_DATA_HOME:-${_avory_home}/.local/share}\"\n"
            "export XDG_STATE_HOME=\"${XDG_STATE_HOME:-${_avory_home}/.local/state}\"\n"
            "unset _avory_home\n";

        FILE *xinit = fopen(TARGET_MOUNT "/root/.xinitrc", "w");
        if (xinit) {
            fputs("#!/bin/sh\n", xinit);
            fputs(xdg_env, xinit);
            fprintf(xinit, "%s\n", xinit_cmd);
            fclose(xinit);
            chmod(TARGET_MOUNT "/root/.xinitrc", 0755);
        }
        if (g_username[0]) {
            char u_xinit[256];
            snprintf(u_xinit, sizeof(u_xinit), TARGET_MOUNT "/home/%s/.xinitrc", g_username);
            xinit = fopen(u_xinit, "w");
            if (xinit) {
                fputs("#!/bin/sh\n", xinit);
                fputs(xdg_env, xinit);
                fprintf(xinit, "%s\n", xinit_cmd);
                fclose(xinit);
                chmod(u_xinit, 0755);
            }
        }
    }

    draw_progress_ui(98, "Flushing filesystem buffers to disk...");
    sync();
    sleep(1);

    draw_progress_ui(100, "Installation completed successfully!");
    usleep(500000);

    /* Unmount cleanly */
    umount2(TARGET_MOUNT "/boot/efi", 2);
    umount2(TARGET_MOUNT, 2);

    return STEP_COMPLETE;
}

/* ──────────────────────────────────────────────────────────────────────────
 * Step 9: Installation Complete!
 * ────────────────────────────────────────────────────────────────────────── */
static wizard_step_t screen_complete(void) {
    int active_btn = 0; /* 0: Reboot, 1: Shell, 2: Exit */
    int box_w = 66;
    int box_h = 16;

    while (1) {
        int box_x = (g_term_cols - box_w) / 2;
        int box_y = (g_term_rows - box_h) / 2;

        tui_draw_backdrop("Finished", 7, 7);
        tui_draw_box(box_y, box_x, box_h, box_w, "Installation Complete", false);

        tui_cursor(box_y + 3, box_x + 6);
        tui_buf_write("\033[1;32;47mAvoryOS has been installed successfully!\033[0m");

        tui_cursor(box_y + 5, box_x + 6);
        tui_buf_write("\033[0;30;47mThe system is now bootable via Limine (UEFI & BIOS).\033[0m");
        tui_cursor(box_y + 6, box_x + 6);
        tui_buf_write("\033[0;30;47mPlease remove any installation USB/CD media before rebooting.\033[0m");

        tui_cursor(box_y + 8, box_x + 6);
        tui_buf_write("\033[0;34;47mSelect an action to continue:\033[0m");

        /* Buttons */
        int btn_y = box_y + box_h - 3;
        tui_draw_button(btn_y, box_x + 8, "Reboot Now", active_btn == 0);
        tui_draw_button(btn_y, box_x + 28, "Chroot Shell", active_btn == 1);
        tui_draw_button(btn_y, box_x + 48, "Exit", active_btn == 2);

        tui_flush();

        int k = tui_read_key();
        if (k == KEY_LEFT || k == KEY_BACKTAB) {
            active_btn = (active_btn + 2) % 3;
        } else if (k == KEY_RIGHT || k == KEY_TAB) {
            active_btn = (active_btn + 1) % 3;
        } else if (k == KEY_ENTER || k == KEY_SPACE) {
            if (active_btn == 0) {
                tui_restore_terminal();
                printf("\n[*] Rebooting system...\n");
                sync();
                system("reboot");
                exit(0);
            }
            if (active_btn == 1) {
                tui_restore_terminal();
                printf("\n[*] Opening shell for manual inspection...\n");
                system("/bin/bash || /bin/sh");
                exit(0);
            }
            if (active_btn == 2) {
                tui_restore_terminal();
                printf("\n[*] Exiting installer. Thank you for choosing AvoryOS!\n");
                exit(0);
            }
        }
    }
}

/* ──────────────────────────────────────────────────────────────────────────
 * Main Wizard Loop
 * ────────────────────────────────────────────────────────────────────────── */
int main(int argc, char **argv) {
    if (argc > 1) {
        if (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0) {
            printf("AvoryOS TTY Installer %s\n"
                   "Usage: %s [options]\n\n"
                   "Options:\n"
                   "  -h, --help       Show this help message\n"
                   "  -v, --version    Show version information\n\n"
                   "Installs AvoryOS to target storage with guided GPT layout,\n"
                   "dual UEFI/BIOS Limine bootloader, and desktop configuration.\n",
                   INSTALLER_VERSION, argv[0]);
            return 0;
        }
        if (strcmp(argv[1], "--version") == 0 || strcmp(argv[1], "-v") == 0) {
            printf("AvoryOS Installer %s\n", INSTALLER_VERSION);
            return 0;
        }
    }

    if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) {
        fprintf(stderr, "avory-installer: interactive terminal TTY required.\n");
        return 1;
    }

    tui_init_terminal();

    wizard_step_t step = STEP_WELCOME;
    while (step != STEP_EXIT) {
        switch (step) {
            case STEP_WELCOME:
                step = screen_welcome();
                break;
            case STEP_KEYMAP:
                step = screen_keymap();
                break;
            case STEP_NETWORK:
                step = screen_network();
                break;
            case STEP_DISK_SELECT:
                step = screen_disk_select();
                break;
            case STEP_CONFIRM_DISK:
                step = screen_confirm_disk();
                break;
            case STEP_COMPONENTS:
                step = screen_components();
                break;
            case STEP_IDENTITY:
                step = screen_identity();
                break;
            case STEP_INSTALLING:
                step = screen_installing();
                break;
            case STEP_COMPLETE:
                step = screen_complete();
                break;
            default:
                step = STEP_EXIT;
                break;
        }
    }

    tui_restore_terminal();
    return 0;
}
