#include "tui.h"
#include "installer.h"

#include <errno.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

#define TUI_MAX_ROWS 120
#define TUI_MAX_COLS 240

typedef struct {
    uint32_t ch;
    uint8_t fg;
    uint8_t bg;
    uint8_t bold;
} tui_cell_t;

int g_term_rows = 25;
int g_term_cols = 80;

static struct termios g_orig_termios;
static bool g_raw_mode = false;
static volatile sig_atomic_t g_resized = 0;

static tui_cell_t g_front[TUI_MAX_ROWS][TUI_MAX_COLS];
static tui_cell_t g_back[TUI_MAX_ROWS][TUI_MAX_COLS];
static bool g_force_redraw = true;

static char g_out_buf[131072];
static size_t g_out_len = 0;

static void out_write(const char *s, size_t n) {
    if (g_out_len + n >= sizeof(g_out_buf)) {
        if (g_out_len > 0) {
            ssize_t unused = write(STDOUT_FILENO, g_out_buf, g_out_len);
            (void)unused;
            g_out_len = 0;
        }
        if (n >= sizeof(g_out_buf)) {
            ssize_t unused = write(STDOUT_FILENO, s, n);
            (void)unused;
            return;
        }
    }
    memcpy(g_out_buf + g_out_len, s, n);
    g_out_len += n;
}

static void out_str(const char *s) {
    out_write(s, strlen(s));
}

static void out_printf(const char *fmt, ...) {
    char tmp[256];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    if (n > 0) {
        size_t len = (size_t)n < sizeof(tmp) ? (size_t)n : sizeof(tmp) - 1;
        out_write(tmp, len);
    }
}

static void out_utf8(uint32_t cp) {
    char u[4];
    size_t n = 0;
    if (cp < 0x80) {
        u[0] = (char)cp;
        n = 1;
    } else if (cp < 0x800) {
        u[0] = (char)(0xC0 | (cp >> 6));
        u[1] = (char)(0x80 | (cp & 0x3F));
        n = 2;
    } else if (cp < 0x10000) {
        u[0] = (char)(0xE0 | (cp >> 12));
        u[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        u[2] = (char)(0x80 | (cp & 0x3F));
        n = 3;
    } else {
        u[0] = (char)(0xF0 | (cp >> 18));
        u[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
        u[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
        u[3] = (char)(0x80 | (cp & 0x3F));
        n = 4;
    }
    out_write(u, n);
}

static uint32_t utf8_next(const char **p) {
    const unsigned char *s = (const unsigned char *)*p;
    if (!s || !*s) return 0;
    uint32_t cp;
    if (s[0] < 0x80) {
        cp = s[0];
        *p += 1;
    } else if ((s[0] & 0xE0) == 0xC0 && (s[1] & 0xC0) == 0x80) {
        cp = ((uint32_t)(s[0] & 0x1F) << 6) | (s[1] & 0x3F);
        *p += 2;
    } else if ((s[0] & 0xF0) == 0xE0 && (s[1] & 0xC0) == 0x80 && (s[2] & 0xC0) == 0x80) {
        cp = ((uint32_t)(s[0] & 0x0F) << 12) | ((uint32_t)(s[1] & 0x3F) << 6) | (s[2] & 0x3F);
        *p += 3;
    } else {
        cp = '?';
        *p += 1;
    }
    return cp;
}

static void update_size(void) {
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_row > 0 && ws.ws_col > 0) {
        g_term_rows = ws.ws_row < TUI_MAX_ROWS ? ws.ws_row : TUI_MAX_ROWS;
        g_term_cols = ws.ws_col < TUI_MAX_COLS ? ws.ws_col : TUI_MAX_COLS;
    }
}

static void sigwinch_handler(int sig) {
    (void)sig;
    g_resized = 1;
    g_force_redraw = true;
}

static void sigint_handler(int sig) {
    (void)sig;
    tui_restore();
    _exit(130);
}

void tui_init(void) {
    if (tcgetattr(STDIN_FILENO, &g_orig_termios) == 0) {
        struct termios raw = g_orig_termios;
        raw.c_iflag &= (tcflag_t)~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
        raw.c_oflag &= (tcflag_t)~(OPOST);
        raw.c_cflag |= CS8;
        raw.c_lflag &= (tcflag_t)~(ECHO | ICANON | IEXTEN);
        raw.c_cc[VMIN] = 1;
        raw.c_cc[VTIME] = 0;
        if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == 0) {
            g_raw_mode = true;
        }
    }

    signal(SIGWINCH, sigwinch_handler);
    signal(SIGINT, sigint_handler);
    signal(SIGTERM, sigint_handler);
    update_size();

    memset(g_front, 0, sizeof(g_front));
    memset(g_back, 0, sizeof(g_back));
    g_force_redraw = true;

    out_str("\033[?1049h\033[?25l\033[2J");
    if (g_out_len > 0) {
        ssize_t unused = write(STDOUT_FILENO, g_out_buf, g_out_len);
        (void)unused;
        g_out_len = 0;
    }
}

void tui_restore(void) {
    if (g_raw_mode) {
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &g_orig_termios);
        g_raw_mode = false;
    }
    const char *seq = "\033[0m\033[?25h\033[?1049l\r\n";
    ssize_t unused = write(STDOUT_FILENO, seq, strlen(seq));
    (void)unused;
}

void tui_clear(uint8_t bg) {
    for (int r = 0; r < g_term_rows; r++) {
        for (int c = 0; c < g_term_cols; c++) {
            g_back[r][c].ch = ' ';
            g_back[r][c].fg = C_TEXT;
            g_back[r][c].bg = bg;
            g_back[r][c].bold = 0;
        }
    }
}

void tui_invalidate(void) {
    g_force_redraw = true;
}

void tui_put_cell(int y, int x, uint8_t fg, uint8_t bg, uint8_t bold, uint32_t ch) {
    if (y < 0 || y >= g_term_rows || x < 0 || x >= g_term_cols) return;
    /* A raw \b, \r or ESC from command output would move the real cursor
     * and leave the screen out of step with the front buffer. */
    if (ch < 0x20 || ch == 0x7F) ch = ' ';
    g_back[y][x].ch = ch;
    g_back[y][x].fg = fg;
    g_back[y][x].bg = bg;
    g_back[y][x].bold = bold;
}

void tui_put_str(int y, int x, uint8_t fg, uint8_t bg, uint8_t bold, const char *s) {
    if (!s || y < 0 || y >= g_term_rows) return;
    int col = x;
    const char *p = s;
    while (*p && col < g_term_cols) {
        uint32_t cp = utf8_next(&p);
        if (col >= 0) tui_put_cell(y, col, fg, bg, bold, cp);
        col++;
    }
}

void tui_put_str_n(int y, int x, int max_w, uint8_t fg, uint8_t bg, uint8_t bold, const char *s) {
    if (!s || y < 0 || y >= g_term_rows || max_w <= 0) return;
    int col = x;
    int limit = x + max_w;
    if (limit > g_term_cols) limit = g_term_cols;
    const char *p = s;
    while (*p && col < limit) {
        uint32_t cp = utf8_next(&p);
        if (col >= 0) tui_put_cell(y, col, fg, bg, bold, cp);
        col++;
    }
}

void tui_printf(int y, int x, uint8_t fg, uint8_t bg, uint8_t bold, const char *fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    tui_put_str(y, x, fg, bg, bold, buf);
}

void tui_fill_rect(int y, int x, int h, int w, uint8_t fg, uint8_t bg, uint32_t ch) {
    for (int r = y; r < y + h; r++) {
        for (int c = x; c < x + w; c++) {
            tui_put_cell(r, c, fg, bg, 0, ch);
        }
    }
}

void tui_flush(void) {
    if (g_resized) {
        g_resized = 0;
        update_size();
        g_force_redraw = true;
    }

    int last_fg = -1, last_bg = -1, last_bold = -1;
    int last_r = -1, last_c = -1;

    for (int r = 0; r < g_term_rows; r++) {
        for (int c = 0; c < g_term_cols; c++) {
            tui_cell_t *b = &g_back[r][c];
            tui_cell_t *f = &g_front[r][c];
            if (!g_force_redraw &&
                b->ch == f->ch && b->fg == f->fg &&
                b->bg == f->bg && b->bold == f->bold) {
                continue;
            }

            if (r != last_r || c != last_c + 1) {
                out_printf("\033[%d;%dH", r + 1, c + 1);
            }
            if (b->bold != last_bold) {
                out_str(b->bold ? "\033[1m" : "\033[22m");
                last_bold = b->bold;
            }
            if (b->fg != last_fg) {
                out_printf("\033[38;5;%um", b->fg);
                last_fg = b->fg;
            }
            if (b->bg != last_bg) {
                out_printf("\033[48;5;%um", b->bg);
                last_bg = b->bg;
            }

            out_utf8(b->ch ? b->ch : ' ');
            *f = *b;
            last_r = r;
            last_c = c;
        }
    }

    g_force_redraw = false;
    if (g_out_len > 0) {
        ssize_t unused = write(STDOUT_FILENO, g_out_buf, g_out_len);
        (void)unused;
        g_out_len = 0;
    }
}

int tui_read_key(void) {
    unsigned char ch = 0;
    ssize_t n;
    while (1) {
        n = read(STDIN_FILENO, &ch, 1);
        if (n < 0 && errno == EINTR) {
            if (g_resized) return KEY_RESIZE;
            continue;
        }
        break;
    }
    if (n <= 0) return KEY_NONE;

    if (ch == 0x0C) { /* Ctrl-L: repaint anything another writer left behind */
        g_force_redraw = true;
        return KEY_RESIZE;
    }
    if (ch == '\r' || ch == '\n') return KEY_ENTER;
    if (ch == 127 || ch == '\b') return KEY_BACKSPACE;
    if (ch == '\t') return KEY_TAB;
    if (ch == ' ') return KEY_SPACE;

    if (ch == 0x1B) {
        struct termios t, old;
        if (tcgetattr(STDIN_FILENO, &old) == 0) {
            t = old;
            t.c_cc[VMIN] = 0;
            t.c_cc[VTIME] = 1;
            tcsetattr(STDIN_FILENO, TCSANOW, &t);
        }
        unsigned char seq[4] = {0};
        ssize_t r1 = read(STDIN_FILENO, &seq[0], 1);
        if (r1 <= 0) {
            tcsetattr(STDIN_FILENO, TCSANOW, &old);
            return KEY_ESC;
        }
        if (seq[0] == '[') {
            ssize_t r2 = read(STDIN_FILENO, &seq[1], 1);
            tcsetattr(STDIN_FILENO, TCSANOW, &old);
            if (r2 > 0) {
                switch (seq[1]) {
                    case 'A': return KEY_UP;
                    case 'B': return KEY_DOWN;
                    case 'C': return KEY_RIGHT;
                    case 'D': return KEY_LEFT;
                    case 'Z': return KEY_BACKTAB;
                    default: break;
                }
            }
            return KEY_NONE;
        }
        tcsetattr(STDIN_FILENO, TCSANOW, &old);
        return KEY_ESC;
    }

    return (int)ch;
}

void tui_draw_box(int y, int x, int h, int w, const char *title, uint8_t border_fg, uint8_t fill_bg) {
    if (h < 2 || w < 2) return;
    tui_fill_rect(y, x, h, w, C_TEXT, fill_bg, ' ');

    tui_put_cell(y, x, border_fg, fill_bg, 0, 0x250C);             /* ┌ */
    tui_put_cell(y, x + w - 1, border_fg, fill_bg, 0, 0x2510);     /* ┐ */
    tui_put_cell(y + h - 1, x, border_fg, fill_bg, 0, 0x2514);     /* └ */
    tui_put_cell(y + h - 1, x + w - 1, border_fg, fill_bg, 0, 0x2518); /* ┘ */

    for (int c = x + 1; c < x + w - 1; c++) {
        tui_put_cell(y, c, border_fg, fill_bg, 0, 0x2500);         /* ─ */
        tui_put_cell(y + h - 1, c, border_fg, fill_bg, 0, 0x2500); /* ─ */
    }
    for (int r = y + 1; r < y + h - 1; r++) {
        tui_put_cell(r, x, border_fg, fill_bg, 0, 0x2502);         /* │ */
        tui_put_cell(r, x + w - 1, border_fg, fill_bg, 0, 0x2502); /* │ */
    }

    if (title && title[0]) {
        char hdr[128];
        snprintf(hdr, sizeof(hdr), " %s ", title);
        tui_put_str_n(y, x + 2, w - 4, C_ACCENT, fill_bg, 1, hdr);
    }
}

void tui_draw_chrome(const char *section, const char *footer_hints) {
    tui_clear(C_BG);

    /* Top bar */
    tui_fill_rect(0, 0, 1, g_term_cols, C_TEXT, C_BAR, ' ');
    tui_put_str(0, 2, C_ACCENT, C_BAR, 1, "avoryinstall");
    tui_printf(0, 15, C_MUTED, C_BAR, 0, "v%s", INSTALLER_VERSION);
    if (section && section[0]) {
        int sx = g_term_cols - (int)strlen(section) - 2;
        if (sx > 24) {
            tui_put_str(0, sx, C_BRIGHT, C_BAR, 1, section);
        }
    }
    for (int c = 0; c < g_term_cols; c++) {
        tui_put_cell(1, c, C_BORDER, C_BG, 0, 0x2500);
    }

    /* Bottom bar */
    int fy = g_term_rows - 1;
    for (int c = 0; c < g_term_cols; c++) {
        tui_put_cell(fy - 1, c, C_BORDER, C_BG, 0, 0x2500);
    }
    tui_fill_rect(fy, 0, 1, g_term_cols, C_MUTED, C_BAR, ' ');
    if (footer_hints && footer_hints[0]) {
        tui_put_str_n(fy, 2, g_term_cols - 4, C_MUTED, C_BAR, 0, footer_hints);
    }
}

void tui_draw_input(int y, int x, int w, const char *val, bool active, bool mask) {
    uint8_t bg = active ? C_SEL_BG : C_BG;
    uint8_t fg = active ? C_SEL_FG : C_TEXT;
    tui_fill_rect(y, x, 1, w, fg, bg, ' ');
    if (!val) return;

    size_t len = strlen(val);
    int max_vis = w - 2;
    size_t start = 0;
    if ((int)len > max_vis && max_vis > 0) {
        start = len - (size_t)max_vis;
    }

    for (size_t i = start; i < len && (int)(i - start) < max_vis; i++) {
        uint32_t ch = mask ? '*' : (unsigned char)val[i];
        tui_put_cell(y, x + 1 + (int)(i - start), fg, bg, active ? 1 : 0, ch);
    }
    if (active) {
        int cur_x = x + 1 + (int)(len - start);
        if (cur_x < x + w - 1) {
            tui_put_cell(y, cur_x, C_ACCENT, bg, 1, '_');
        }
    }
}

void tui_draw_button(int y, int x, const char *label, bool active, bool danger) {
    uint8_t bg = active ? (danger ? C_ERR : C_ACCENT) : C_PANEL;
    uint8_t fg = active ? C_BG : (danger ? C_ERR : C_TEXT);
    tui_printf(y, x, fg, bg, active ? 1 : 0, " [ %s ] ", label);
}

void tui_modal_message(const char *title, const char *line1, const char *line2, bool is_error) {
    int w = 68;
    if (w > g_term_cols - 4) w = g_term_cols - 4;
    int h = 10;
    int x = (g_term_cols - w) / 2;
    int y = (g_term_rows - h) / 2;

    while (1) {
        tui_draw_box(y, x, h, w, title, is_error ? C_ERR : C_ACCENT, C_PANEL);
        if (line1) tui_put_str_n(y + 3, x + 3, w - 6, is_error ? C_ERR : C_TEXT, C_PANEL, 1, line1);
        if (line2) tui_put_str_n(y + 5, x + 3, w - 6, C_MUTED, C_PANEL, 0, line2);
        tui_draw_button(y + h - 3, x + (w - 8) / 2, "OK", true, is_error);
        tui_flush();

        int k = tui_read_key();
        if (k == KEY_ENTER || k == KEY_ESC || k == KEY_SPACE) break;
    }
}

bool tui_modal_confirm(const char *title, const char *line1, const char *line2, bool danger) {
    int w = 68;
    if (w > g_term_cols - 4) w = g_term_cols - 4;
    int h = 11;
    int x = (g_term_cols - w) / 2;
    int y = (g_term_rows - h) / 2;
    int sel = 0; /* 0 = Cancel, 1 = Confirm */

    while (1) {
        tui_draw_box(y, x, h, w, title, danger ? C_ERR : C_ACCENT, C_PANEL);
        if (line1) tui_put_str_n(y + 3, x + 3, w - 6, danger ? C_WARN : C_TEXT, C_PANEL, 1, line1);
        if (line2) tui_put_str_n(y + 5, x + 3, w - 6, C_MUTED, C_PANEL, 0, line2);

        tui_draw_button(y + h - 3, x + w / 2 - 15, "Cancel", sel == 0, false);
        tui_draw_button(y + h - 3, x + w / 2 + 2, danger ? "Confirm Wipe" : "Confirm", sel == 1, danger);
        tui_flush();

        int k = tui_read_key();
        if (k == KEY_ESC) return false;
        if (k == KEY_LEFT || k == KEY_RIGHT || k == KEY_TAB || k == KEY_BACKTAB) {
            sel = !sel;
        } else if (k == KEY_ENTER) {
            return sel == 1;
        }
    }
}

bool tui_prompt_text(const char *title, const char *label, char *buf, size_t buf_sz, bool mask, bool allow_empty) {
    char tmp[128];
    snprintf(tmp, sizeof(tmp), "%s", buf);
    size_t len = strlen(tmp);

    int w = 64;
    if (w > g_term_cols - 4) w = g_term_cols - 4;
    int h = 11;
    int x = (g_term_cols - w) / 2;
    int y = (g_term_rows - h) / 2;

    while (1) {
        tui_draw_box(y, x, h, w, title, C_ACCENT, C_PANEL);
        tui_put_str_n(y + 3, x + 3, w - 6, C_TEXT, C_PANEL, 0, label);
        tui_draw_input(y + 5, x + 3, w - 6, tmp, true, mask);
        tui_put_str_n(y + 8, x + 3, w - 6, C_MUTED, C_PANEL, 0, "Enter: Save   Esc: Cancel");
        tui_flush();

        int k = tui_read_key();
        if (k == KEY_ESC) return false;
        if (k == KEY_ENTER) {
            if (!allow_empty && len == 0) {
                tui_modal_message("Validation Error", "Value cannot be empty.", NULL, true);
                continue;
            }
            snprintf(buf, buf_sz, "%s", tmp);
            return true;
        }
        if (k == KEY_BACKSPACE) {
            if (len > 0) tmp[--len] = '\0';
        } else if (k >= 32 && k < 127 && len + 1 < sizeof(tmp) && len + 1 < buf_sz) {
            tmp[len++] = (char)k;
            tmp[len] = '\0';
        }
    }
}
