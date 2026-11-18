#ifndef AVORY_INSTALLER_TUI_H
#define AVORY_INSTALLER_TUI_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Dark Archinstall-inspired xterm-256 palette */
#define C_BG      233 /* #121212 deep charcoal */
#define C_PANEL   235 /* #262626 dark card */
#define C_BAR     234 /* #1c1c1c header/footer bar */
#define C_BORDER  240 /* #585858 muted border */
#define C_TEXT    252 /* #d0d0d0 crisp light gray */
#define C_BRIGHT  255 /* #ffffff bright white */
#define C_MUTED   244 /* #808080 dim gray */
#define C_ACCENT   39 /* #00afff Arch cyan-blue */
#define C_SEL_BG   24 /* #005f87 dark cyan selection */
#define C_SEL_FG  255 /* #ffffff */
#define C_OK       78 /* #5fd787 soft green */
#define C_WARN    220 /* #ffd700 amber */
#define C_ERR     203 /* #ff5f5f coral red */

enum {
    KEY_NONE = 0,
    KEY_UP = 1000,
    KEY_DOWN,
    KEY_LEFT,
    KEY_RIGHT,
    KEY_ENTER,
    KEY_ESC,
    KEY_BACKSPACE,
    KEY_TAB,
    KEY_BACKTAB,
    KEY_SPACE,
    KEY_RESIZE,
};

extern int g_term_rows;
extern int g_term_cols;

void tui_init(void);
void tui_restore(void);
void tui_clear(uint8_t bg);
void tui_flush(void);
/* Repaint every cell on the next flush (after another process drew). */
void tui_invalidate(void);
int tui_read_key(void);

void tui_put_cell(int y, int x, uint8_t fg, uint8_t bg, uint8_t bold, uint32_t ch);
void tui_put_str(int y, int x, uint8_t fg, uint8_t bg, uint8_t bold, const char *s);
void tui_put_str_n(int y, int x, int max_w, uint8_t fg, uint8_t bg, uint8_t bold, const char *s);
void tui_printf(int y, int x, uint8_t fg, uint8_t bg, uint8_t bold, const char *fmt, ...);
void tui_fill_rect(int y, int x, int h, int w, uint8_t fg, uint8_t bg, uint32_t ch);

void tui_draw_box(int y, int x, int h, int w, const char *title, uint8_t border_fg, uint8_t fill_bg);
void tui_draw_chrome(const char *section, const char *footer_hints);
void tui_draw_input(int y, int x, int w, const char *val, bool active, bool mask);
void tui_draw_button(int y, int x, const char *label, bool active, bool danger);

void tui_modal_message(const char *title, const char *line1, const char *line2, bool is_error);
bool tui_modal_confirm(const char *title, const char *line1, const char *line2, bool danger);
bool tui_prompt_text(const char *title, const char *label, char *buf, size_t buf_sz, bool mask, bool allow_empty);

#endif /* AVORY_INSTALLER_TUI_H */
