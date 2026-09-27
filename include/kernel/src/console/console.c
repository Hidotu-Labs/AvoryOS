#include "console.h"
#include "../apic/lapic_timer.h"
#include "../drivers/input/keyboard.h"
#include "../drivers/serial.h"
#include "fb/framebuffer.h"
#include "font/font.h"
#include "lib/string.h"
#include "lock/spinlock.h"
#include <stddef.h>
#include <stdint.h>

static spinlock_t console_lock = SPINLOCK_INIT;
static void console_set_cursor_visible_unlocked(bool visible);
static void console_refresh_cursor_unlocked(void);
static uint32_t console_history_row(uint32_t screen_row);
static void console_process_escape_sequence(void);
static void console_clear_line_from_cursor(void);
static void draw_char_colored(uint32_t c, uint32_t col, uint32_t row,
                              uint32_t fg, uint32_t bg, bool underline);
static void draw_history_char(uint32_t col, uint32_t row);
static void draw_cursor_block(uint32_t col, uint32_t row);
static void console_wipe_history_unlocked(void);

static bool terminal_escape = false;
static char terminal_escape_buffer[64];
static size_t terminal_escape_len = 0;
// Operating System Command (OSC) strings, such as OSC 8 hyperlinks, are not
// useful on the framebuffer console. Consume them through their BEL or ST
// terminator so their payload is not rendered as ordinary text.
static bool terminal_osc = false;
static bool terminal_osc_esc = false;

// UTF-8 multi-byte decoder state
static uint32_t utf8_codepoint = 0;
static int utf8_bytes_remaining = 0;
static void console_render_char(uint32_t cp);

#define BG_COLOR 0x00000000
#define FG_COLOR 0x00CDD6F4

// ANSI color palette (Catppuccin Mocha)
static uint32_t ansi_colors_normal[8] = {
    0x00181825, // 0 black   (surface0)
    0x00F38BA8, // 1 red
    0x00A6E3A1, // 2 green
    0x00F9E2AF, // 3 yellow
    0x0089B4FA, // 4 blue
    0x00CBA4F7, // 5 magenta
    0x0094E2D5, // 6 cyan
    0x00CDD6F4, // 7 white
};

static uint32_t ansi_colors_bright[8] = {
    0x00313244, // 0 bright black  (surface1)
    0x00F38BA8, // 1 bright red
    0x00A6E3A1, // 2 bright green
    0x00F9E2AF, // 3 bright yellow
    0x0089B4FA, // 4 bright blue
    0x00CBA4F7, // 5 bright magenta
    0x0094E2D5, // 6 bright cyan
    0x00FFFFFF, // 7 bright white
};

// Current SGR color state
static uint32_t current_fg = FG_COLOR;
static uint32_t current_bg = BG_COLOR;
static bool attr_bold = false;
static bool attr_underline = false;

typedef struct {
  uint32_t c;
  uint32_t fg;
  uint32_t bg;
  bool bold;
  bool underline;
} console_char_t;

// 256-color palette helper
// Returns the 32-bit RGB value for an xterm 256-color index.
static uint32_t xterm256_color(int idx) {
  if (idx >= 0 && idx <= 7)
    return ansi_colors_normal[idx];
  if (idx >= 8 && idx <= 15)
    return ansi_colors_bright[idx - 8];
  // 6×6×6 color cube: indices 16–231
  if (idx >= 16 && idx <= 231) {
    int i = idx - 16;
    uint32_t b = i % 6;
    uint32_t g = (i / 6) % 6;
    uint32_t r = i / 36;
    // Each component: 0→0, 1→95, 2→135, 3→175, 4→215, 5→255
    static const uint8_t ramp[6] = {0, 95, 135, 175, 215, 255};
    return ((uint32_t)ramp[r] << 16) | ((uint32_t)ramp[g] << 8) | ramp[b];
  }
  // Grayscale ramp: indices 232–255 → #080808 to #eeeeee in steps of 10
  if (idx >= 232 && idx <= 255) {
    uint8_t v = (uint8_t)(8 + (idx - 232) * 10);
    return ((uint32_t)v << 16) | ((uint32_t)v << 8) | v;
  }
  return FG_COLOR;
}

// Returns the bold-brightened variant of a named ANSI color, or the color
// unchanged if it isn't one of the 8 named normal colors.
static uint32_t bold_color(uint32_t color) {
  for (int i = 0; i < 8; i++) {
    if (color == ansi_colors_normal[i])
      return ansi_colors_bright[i];
  }
  return color;
}

#define HISTORY_MAX 1000
#define COLS_MAX 512

static console_char_t history[HISTORY_MAX][COLS_MAX];
static uint32_t history_write_row = 0;
static uint32_t view_scroll_offset = 0;

static uint32_t cursor_x;
static uint32_t cursor_y;
static uint32_t max_cols;
static uint32_t max_rows;
static bool cursor_logical_visible = false;
static bool cursor_phys_on = false;
static uint64_t last_blink_ms = 0;
static bool cursor_repositioned =
    false; // Set when CUP/ESC[H moves cursor above bottom
static bool wrap_pending = false; // Deferred line wrap (autowrap pending)
static bool acs_active = false;   // true when SO (G1 line-drawing) is selected
static uint32_t saved_cursor_x = 0;
static uint32_t saved_cursor_y = 0;
static uint32_t scroll_region_top = 0;
static uint32_t scroll_region_bottom = 0; // 0 means "use max_rows-1"
static uint32_t last_printed_cp = ' ';    // for REP (ESC[nb)

static void console_redraw(void) {
  fb_set_backbuffer_mode(true);
  fb_clear(BG_COLOR);

  uint32_t end_row = history_write_row - view_scroll_offset;
  uint32_t start_row =
      (end_row >= max_rows - 1) ? (end_row - (max_rows - 1)) : 0;

  for (uint32_t y = 0; y < max_rows; y++) {
    uint32_t h_row = start_row + y;
    if (h_row > end_row)
      break;

    for (uint32_t x = 0; x < max_cols; x++) {
      console_char_t *ch = &history[h_row % HISTORY_MAX][x];
      uint32_t px = x * FONT_WIDTH;
      uint32_t py = y * FONT_HEIGHT;

      if (ch->c != 0) {
        const uint8_t *glyph = font_get_glyph(ch->c);
        for (uint32_t gy = 0; gy < FONT_HEIGHT; gy++) {
          uint8_t bits =
              (ch->underline && gy >= FONT_HEIGHT - 2) ? 0xFF : glyph[gy];
          for (uint32_t gx = 0; gx < FONT_WIDTH; gx++) {
            uint32_t color = (bits & (0x80 >> gx)) ? ch->fg : ch->bg;
            fb_put_pixel(px + gx, py + gy, color);
          }
        }
      } else if (ch->bg != BG_COLOR) {
        // Cell is blank but has a non-default background (e.g. reverse video
        // status bar)
        fb_fill_rect(px, py, FONT_WIDTH, FONT_HEIGHT, ch->bg);
      }
    }
  }

  if (view_scroll_offset == 0 && cursor_logical_visible && cursor_phys_on)
    draw_cursor_block(cursor_x, cursor_y);

  fb_swap_buffer();
  fb_set_backbuffer_mode(false);
}

void console_scroll_view(int delta) {
  spinlock_acquire(&console_lock);

  int new_offset = (int)view_scroll_offset + delta;
  if (new_offset < 0)
    new_offset = 0;

  uint32_t max_scroll = (history_write_row > (max_rows - 1))
                            ? (history_write_row - (max_rows - 1))
                            : 0;
  if (max_scroll > HISTORY_MAX - max_rows)
    max_scroll = HISTORY_MAX - max_rows;

  if ((uint32_t)new_offset > max_scroll)
    new_offset = max_scroll;

  if ((uint32_t)new_offset != view_scroll_offset) {
    view_scroll_offset = (uint32_t)new_offset;
    console_redraw();
  }

  spinlock_release(&console_lock);
}

uint32_t console_get_rows(void) { return max_rows; }

static void scroll_up(void) {
  uint32_t fb_w = fb_get_width();
  uint32_t fb_h = fb_get_height();
  uint32_t pitch = fb_get_pitch();
  void *base = fb_is_backbuffer_enabled() ? fb_get_backbuffer() : fb_get_base();

  uint32_t move_height = fb_h - FONT_HEIGHT;
  uint64_t bytes_to_copy = (uint64_t)move_height * pitch;

  uint8_t *dst = (uint8_t *)base;
  uint8_t *src = (uint8_t *)base + (FONT_HEIGHT * pitch);

  memcpy(dst, src, bytes_to_copy);
  fb_mark_dirty(0, 0, fb_w, move_height);

  /* A terminal scroll exposes blank cells using the current rendition.
   * Full-screen programs (including nyancat) commonly keep a non-default
   * background selected while repainting. */
  fb_fill_rect(0, fb_h - FONT_HEIGHT, fb_w, FONT_HEIGHT, current_bg);

  if (cursor_y > 0) {
    cursor_y--;
  }
}

void console_init(struct limine_framebuffer *framebuffer) {
  fb_init(framebuffer);

  max_cols = fb_get_width() / FONT_WIDTH;
  max_rows = fb_get_height() / FONT_HEIGHT;
  if (max_cols > COLS_MAX)
    max_cols = COLS_MAX;
  if (max_rows > HISTORY_MAX)
    max_rows = HISTORY_MAX;
  cursor_x = 0;
  cursor_y = 0;
  terminal_escape = false;
  terminal_escape_len = 0;
  cursor_repositioned = false;
  wrap_pending = false;
  acs_active = false;
  scroll_region_top = 0;
  scroll_region_bottom = 0;
  saved_cursor_x = 0;
  saved_cursor_y = 0;
  last_printed_cp = ' ';

  fb_clear(BG_COLOR);
}

static uint32_t console_history_row(uint32_t screen_row) {
  if (history_write_row >= max_rows - 1) {
    return (history_write_row - (max_rows - 1) + screen_row) % HISTORY_MAX;
  }
  return screen_row % HISTORY_MAX;
}

static void console_clear_line_from_cursor(void) {
  uint32_t row = console_history_row(cursor_y);
  for (uint32_t x = cursor_x; x < max_cols; x++) {
    console_char_t *cell = &history[row][x];
    cell->c = 0;
    cell->fg = current_fg;
    cell->bg = current_bg;
    if (view_scroll_offset == 0) {
      fb_fill_rect(x * FONT_WIDTH, cursor_y * FONT_HEIGHT, FONT_WIDTH,
                   FONT_HEIGHT, current_bg);
    }
  }
}

static void console_wipe_history_unlocked(void) {
  /* Keep the front and back buffers synchronized. Clearing only the front
     buffer allowed the next text swap to restore stale boot output and the
     cursor drawn at its old position. */
  if (fb_get_backbuffer()) {
    fb_set_backbuffer_mode(true);
    fb_clear(BG_COLOR);
    fb_swap_buffer();
    fb_set_backbuffer_mode(false);
  } else {
    fb_set_backbuffer_mode(false);
    fb_clear(BG_COLOR);
  }
  memset(history, 0, sizeof(history));
  cursor_x = 0;
  cursor_y = 0;
  view_scroll_offset = 0;
  history_write_row = 0;
  cursor_logical_visible = false;
  cursor_phys_on = false;
  cursor_repositioned = false;
  wrap_pending = false;
  acs_active = false;
  scroll_region_top = 0;
  scroll_region_bottom = 0;
  last_printed_cp = ' ';
  current_fg = FG_COLOR;
  current_bg = BG_COLOR;
  attr_bold = false;
  attr_underline = false;
}

// VT100 ACS (Alternate Character Set) → Unicode mapping.
// When G1 is active (SO), bytes 0x60–0x7E map to line-drawing characters.
static const uint32_t acs_map[0x20] = {
    // 0x60  `  → ◆ DIAMOND
    0x25C6,
    // 0x61  a  → ░ MEDIUM SHADE (checkerboard)
    0x2592,
    // 0x62  b  → HT (no glyph, render space)
    0x0020,
    // 0x63  c  → FF (no glyph, render space)
    0x0020,
    // 0x64  d  → CR (no glyph, render space)
    0x0020,
    // 0x65  e  → LF (no glyph, render space)
    0x0020,
    // 0x66  f  → ° DEGREE SIGN
    0x00B0,
    // 0x67  g  → ± PLUS-MINUS SIGN
    0x00B1,
    // 0x68  h  → NL (no glyph, render space)
    0x0020,
    // 0x69  i  → VT (no glyph, render space)
    0x0020,
    // 0x6A  j  → ┘ BOX DRAWINGS LIGHT UP AND LEFT
    0x2518,
    // 0x6B  k  → ┐ BOX DRAWINGS LIGHT DOWN AND LEFT
    0x2510,
    // 0x6C  l  → ┌ BOX DRAWINGS LIGHT DOWN AND RIGHT
    0x250C,
    // 0x6D  m  → └ BOX DRAWINGS LIGHT UP AND RIGHT
    0x2514,
    // 0x6E  n  → ┼ BOX DRAWINGS LIGHT VERTICAL AND HORIZONTAL
    0x253C,
    // 0x6F  o  → ⎺ HORIZONTAL SCAN LINE 1 (use overline approx)
    0x23BA,
    // 0x70  p  → ⎻ HORIZONTAL SCAN LINE 3
    0x23BB,
    // 0x71  q  → ─ BOX DRAWINGS LIGHT HORIZONTAL
    0x2500,
    // 0x72  r  → ⎼ HORIZONTAL SCAN LINE 7
    0x23BC,
    // 0x73  s  → ⎽ HORIZONTAL SCAN LINE 9
    0x23BD,
    // 0x74  t  → ├ BOX DRAWINGS LIGHT VERTICAL AND RIGHT
    0x251C,
    // 0x75  u  → ┤ BOX DRAWINGS LIGHT VERTICAL AND LEFT
    0x2524,
    // 0x76  v  → ┴ BOX DRAWINGS LIGHT UP AND HORIZONTAL
    0x2534,
    // 0x77  w  → ┬ BOX DRAWINGS LIGHT DOWN AND HORIZONTAL
    0x252C,
    // 0x78  x  → │ BOX DRAWINGS LIGHT VERTICAL
    0x2502,
    // 0x79  y  → ≤ LESS-THAN OR EQUAL TO
    0x2264,
    // 0x7A  z  → ≥ GREATER-THAN OR EQUAL TO
    0x2265,
    // 0x7B  {  → π PI
    0x03C0,
    // 0x7C  |  → ≠ NOT EQUAL TO
    0x2260,
    // 0x7D  }  → £ POUND SIGN
    0x00A3,
    // 0x7E  ~  → · MIDDLE DOT
    0x00B7,
};

static uint32_t scroll_bottom(void) {
  return (scroll_region_bottom > 0 && scroll_region_bottom < max_rows)
             ? scroll_region_bottom
             : max_rows - 1;
}

static void console_process_escape_sequence(void) {
  if (terminal_escape_len == 0)
    return;
  if (terminal_escape_buffer[0] != '[')
    return;

  bool question = false;
  int params[16] = {0};
  int param_count = 1;
  char *p = terminal_escape_buffer + 1;

  if (*p == '?') {
    question = true;
    p++;
  }

  while (*p && (*p < '@' || *p > '~')) {
    if (*p >= '0' && *p <= '9') {
      params[param_count - 1] = params[param_count - 1] * 10 + (*p - '0');
    } else if (*p == ';') {
      if (param_count < 16)
        param_count++;
    }
    p++;
  }

  char final = *p;
  int value = params[0];
  int value2 = (param_count > 1) ? params[1] : 0;

  switch (final) {
  // Cursor movement
  case 'A':
    if (value == 0)
      value = 1;
    cursor_y = (cursor_y >= (uint32_t)value) ? cursor_y - value : 0;
    wrap_pending = false;
    break;
  case 'B':
    cursor_y += value ? value : 1;
    if (cursor_y >= max_rows)
      cursor_y = max_rows - 1;
    wrap_pending = false;
    break;
  case 'C':
    cursor_x += value ? value : 1;
    if (cursor_x >= max_cols)
      cursor_x = max_cols - 1;
    wrap_pending = false;
    break;
  case 'D':
    if (value == 0)
      value = 1;
    cursor_x = (cursor_x >= (uint32_t)value) ? cursor_x - value : 0;
    wrap_pending = false;
    break;
  case 'E': // Cursor Next Line
    cursor_y += value ? value : 1;
    if (cursor_y >= max_rows)
      cursor_y = max_rows - 1;
    cursor_x = 0;
    wrap_pending = false;
    break;
  case 'F': // Cursor Previous Line
    if (value == 0)
      value = 1;
    cursor_y = (cursor_y >= (uint32_t)value) ? cursor_y - value : 0;
    cursor_x = 0;
    wrap_pending = false;
    break;
  case 'G': // Cursor Horizontal Absolute
    cursor_x = (value > 0) ? (uint32_t)(value - 1) : 0;
    if (cursor_x >= max_cols)
      cursor_x = max_cols - 1;
    wrap_pending = false;
    break;
  case 'H':
  case 'f': {
    uint32_t new_y = (value > 0) ? (uint32_t)(value - 1) : 0;
    cursor_x = (value2 > 0) ? (uint32_t)(value2 - 1) : 0;
    if (new_y >= max_rows)
      new_y = max_rows - 1;
    if (cursor_x >= max_cols)
      cursor_x = max_cols - 1;
    // If cursor is moved above the current content bottom, mark as
    // repositioned so that subsequent newlines don't advance history.
    uint32_t bottom =
        (history_write_row >= max_rows - 1) ? max_rows - 1 : history_write_row;
    if (new_y < bottom)
      cursor_repositioned = true;
    cursor_y = new_y;
    wrap_pending = false;
    break;
  }

  // Erase
  case 'J':
    if (value == 0) {
      // Erase from cursor to end of screen
      console_clear_line_from_cursor();
      for (uint32_t y = cursor_y + 1; y < max_rows; y++) {
        uint32_t row = console_history_row(y);
        for (uint32_t x = 0; x < max_cols; x++) {
          history[row][x].c = 0;
          history[row][x].fg = current_fg;
          history[row][x].bg = current_bg;
        }
        if (view_scroll_offset == 0) {
          fb_fill_rect(0, y * FONT_HEIGHT, fb_get_width(), FONT_HEIGHT,
                       current_bg);
        }
      }
    } else if (value == 1) {
      // Erase from beginning of screen to cursor
      for (uint32_t y = 0; y < cursor_y; y++) {
        uint32_t row = console_history_row(y);
        for (uint32_t x = 0; x < max_cols; x++) {
          history[row][x].c = 0;
          history[row][x].fg = current_fg;
          history[row][x].bg = current_bg;
        }
        if (view_scroll_offset == 0) {
          fb_fill_rect(0, y * FONT_HEIGHT, fb_get_width(), FONT_HEIGHT,
                       current_bg);
        }
      }
      uint32_t row = console_history_row(cursor_y);
      for (uint32_t x = 0; x <= cursor_x && x < max_cols; x++) {
        history[row][x].c = 0;
        history[row][x].fg = current_fg;
        history[row][x].bg = current_bg;
        if (view_scroll_offset == 0) {
          fb_fill_rect(x * FONT_WIDTH, cursor_y * FONT_HEIGHT, FONT_WIDTH,
                       FONT_HEIGHT, current_bg);
        }
      }
    } else if (value == 2) {
      // Erase entire viewport but keep scrollback
      for (uint32_t y = 0; y < max_rows; y++) {
        uint32_t row = console_history_row(y);
        for (uint32_t x = 0; x < max_cols; x++) {
          history[row][x].c = 0;
          history[row][x].fg = current_fg;
          history[row][x].bg = current_bg;
        }
        if (view_scroll_offset == 0) {
          fb_fill_rect(0, y * FONT_HEIGHT, fb_get_width(), FONT_HEIGHT,
                       current_bg);
        }
      }
      cursor_x = 0;
      cursor_y = 0;
    } else if (value == 3) {
      // Erase entire screen AND scrollback (history)
      console_wipe_history_unlocked();
    }
    break;
  case 'K':
    if (value == 0) {
      // Erase from cursor to end of line
      console_clear_line_from_cursor();
    } else if (value == 1) {
      // Erase from beginning of line to cursor
      uint32_t row = console_history_row(cursor_y);
      for (uint32_t x = 0; x <= cursor_x && x < max_cols; x++) {
        history[row][x].c = 0;
        history[row][x].fg = current_fg;
        history[row][x].bg = current_bg;
        if (view_scroll_offset == 0) {
          fb_fill_rect(x * FONT_WIDTH, cursor_y * FONT_HEIGHT, FONT_WIDTH,
                       FONT_HEIGHT, current_bg);
        }
      }
    } else if (value == 2) {
      // Erase entire line
      uint32_t row = console_history_row(cursor_y);
      for (uint32_t x = 0; x < max_cols; x++) {
        history[row][x].c = 0;
        history[row][x].fg = current_fg;
        history[row][x].bg = current_bg;
        if (view_scroll_offset == 0) {
          fb_fill_rect(x * FONT_WIDTH, cursor_y * FONT_HEIGHT, FONT_WIDTH,
                       FONT_HEIGHT, current_bg);
        }
      }
    }
    break;

  // Cursor visibility
  case 'h':
    if (question) {
      if (value == 25)
        console_set_cursor_visible_unlocked(true);
      // ?7h = DECAWM enable (autowrap on — default, no-op since we always do
      // deferred wrap) ?1049h = alternate screen (no-op, we don't have a
      // separate screen buffer) ?2004h = bracketed paste mode (no-op)
    }
    break;
  case 'l':
    if (question) {
      if (value == 25)
        console_set_cursor_visible_unlocked(false);
      // ?7l = DECAWM disable — disable the deferred autowrap
      // (we leave wrap_pending as-is; full-screen apps manage line endings
      // themselves) ?1049l = alternate screen exit (no-op) ?2004l = bracketed
      // paste mode off (no-op)
    }
    break;

  // Cursor position report
  case 'n':
    if (!question && value == 6) {
      char resp[32];
      int len = 0;
      char temp[16];
      uint32_t row = cursor_y + 1;
      uint32_t col = cursor_x + 1;
      resp[len++] = 0x1B;
      resp[len++] = '[';
      if (row == 0) {
        resp[len++] = '0';
      } else {
        int digits = 0;
        uint32_t v = row;
        while (v > 0) {
          temp[digits++] = '0' + (v % 10);
          v /= 10;
        }
        while (digits--)
          resp[len++] = temp[digits];
      }
      resp[len++] = ';';
      if (col == 0) {
        resp[len++] = '0';
      } else {
        int digits = 0;
        uint32_t v = col;
        while (v > 0) {
          temp[digits++] = '0' + (v % 10);
          v /= 10;
        }
        while (digits--)
          resp[len++] = temp[digits];
      }
      resp[len++] = 'R';
      keyboard_push_bytes(resp, (uint32_t)len);
    }
    break;

  // SGR — Select Graphic Rendition (colors + attributes)
  case 'm': {
    // Bare ESC[m is equivalent to ESC[0m — full reset
    if (param_count == 1 && params[0] == 0) {
      current_fg = FG_COLOR;
      current_bg = BG_COLOR;
      attr_bold = false;
      attr_underline = false;
      break;
    }

    for (int pi = 0; pi < param_count; pi++) {
      int code = params[pi];

      if (code == 0) {
        // Reset all attributes
        current_fg = FG_COLOR;
        current_bg = BG_COLOR;
        attr_bold = false;
        attr_underline = false;
      } else if (code == 1) {
        // Bold — switch to bright variant of named colors
        attr_bold = true;
        current_fg = bold_color(current_fg);
      } else if (code == 2) {
        // Faint/dim — no-op (no dim palette)
      } else if (code == 3) {
        // Italic — no-op (bitmap font has no italic variant)
      } else if (code == 4) {
        // Underline
        attr_underline = true;
      } else if (code == 5 || code == 6) {
        // Blink — no-op
      } else if (code == 7) {
        // Reverse video — swap fg and bg
        uint32_t tmp = current_fg;
        current_fg = current_bg;
        current_bg = tmp;
      } else if (code == 22) {
        // Normal intensity — turn off bold, revert to normal palette entry
        if (attr_bold) {
          attr_bold = false;
          // Try to revert bright → normal for named colors
          for (int i = 0; i < 8; i++) {
            if (current_fg == ansi_colors_bright[i]) {
              current_fg = ansi_colors_normal[i];
              break;
            }
          }
        }
      } else if (code == 23) {
        // Italic off — no-op
      } else if (code == 24) {
        // Underline off
        attr_underline = false;
      } else if (code == 27) {
        // Reverse off — reset to defaults
        current_fg = FG_COLOR;
        current_bg = BG_COLOR;
      } else if (code >= 30 && code <= 37) {
        current_fg = attr_bold ? ansi_colors_bright[code - 30]
                               : ansi_colors_normal[code - 30];
      } else if (code == 38) {
        // 256-color or truecolor fg
        if (pi + 1 < param_count && params[pi + 1] == 5 &&
            pi + 2 < param_count) {
          // ESC[38;5;Nm — 256-color
          current_fg = xterm256_color(params[pi + 2]);
          pi += 2;
        } else if (pi + 1 < param_count && params[pi + 1] == 2 &&
                   pi + 4 < param_count) {
          // ESC[38;2;R;G;Bm — truecolor
          uint8_t r = (uint8_t)params[pi + 2];
          uint8_t g = (uint8_t)params[pi + 3];
          uint8_t b = (uint8_t)params[pi + 4];
          current_fg = ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
          pi += 4;
        }
      } else if (code == 39) {
        current_fg = attr_bold ? bold_color(FG_COLOR) : FG_COLOR;
      } else if (code >= 40 && code <= 47) {
        current_bg = ansi_colors_normal[code - 40];
      } else if (code == 48) {
        // 256-color or truecolor bg
        if (pi + 1 < param_count && params[pi + 1] == 5 &&
            pi + 2 < param_count) {
          current_bg = xterm256_color(params[pi + 2]);
          pi += 2;
        } else if (pi + 1 < param_count && params[pi + 1] == 2 &&
                   pi + 4 < param_count) {
          uint8_t r = (uint8_t)params[pi + 2];
          uint8_t g = (uint8_t)params[pi + 3];
          uint8_t b = (uint8_t)params[pi + 4];
          current_bg = ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
          pi += 4;
        }
      } else if (code == 49) {
        current_bg = BG_COLOR;
      } else if (code >= 90 && code <= 97) {
        current_fg = ansi_colors_bright[code - 90];
      } else if (code >= 100 && code <= 107) {
        current_bg = ansi_colors_bright[code - 100];
      }
    }
    break;
  }

  case 'd': // Cursor Vertical Absolute — move cursor to row N
    cursor_y = (value > 0) ? (uint32_t)(value - 1) : 0;
    if (cursor_y >= max_rows)
      cursor_y = max_rows - 1;
    break;

  case 'r': // Set scroll region (DECSTBM): ESC[top;bottom r
    scroll_region_top = (value > 0) ? (uint32_t)(value - 1) : 0;
    scroll_region_bottom = (value2 > 0) ? (uint32_t)(value2 - 1) : 0;
    if (scroll_region_top >= max_rows)
      scroll_region_top = 0;
    if (scroll_region_bottom >= max_rows)
      scroll_region_bottom = max_rows - 1;
    // Reset cursor to home on scroll region change (xterm behaviour)
    cursor_x = 0;
    cursor_y = scroll_region_top;
    wrap_pending = false;
    break;

  case 'S': { // Scroll Up N lines
    int n = (value > 0) ? value : 1;
    for (int i = 0; i < n; i++) {
      scroll_up();
      history_write_row++;
      uint32_t new_row = history_write_row % HISTORY_MAX;
      for (uint32_t x = 0; x < COLS_MAX; x++) {
        history[new_row][x].c = 0;
        history[new_row][x].fg = FG_COLOR;
        history[new_row][x].bg = BG_COLOR;
      }
    }
    break;
  }

  case 'X': { // ECH — Erase Character (paint N cells with bg, don't move
              // cursor)
    int n = (value > 0) ? value : 1;
    uint32_t row = console_history_row(cursor_y);
    for (int i = 0; i < n && cursor_x + (uint32_t)i < max_cols; i++) {
      uint32_t cx = cursor_x + (uint32_t)i;
      history[row][cx].c = 0;
      history[row][cx].fg = current_fg;
      history[row][cx].bg = current_bg;
      if (view_scroll_offset == 0)
        fb_fill_rect(cx * FONT_WIDTH, cursor_y * FONT_HEIGHT, FONT_WIDTH,
                     FONT_HEIGHT, current_bg);
    }
    break;
  }

  case 'b': { // REP — Repeat Preceding Graphic Character N times
    int n = (value > 0) ? value : 1;
    // Repeat the last character that was actually drawn (tracked below)
    // We implement this by re-rendering console_render_char for the last cp.
    for (int i = 0; i < n; i++)
      console_render_char(last_printed_cp);
    break;
  }

  case 'P': { // Delete N characters (DCH)
    int n = (value > 0) ? value : 1;
    uint32_t row = console_history_row(cursor_y);
    uint32_t end = max_cols - (uint32_t)n;
    for (uint32_t x = cursor_x; x < max_cols; x++) {
      if (x + (uint32_t)n < max_cols) {
        history[row][x] = history[row][x + (uint32_t)n];
      } else {
        history[row][x].c = 0;
        history[row][x].fg = current_fg;
        history[row][x].bg = current_bg;
      }
    }
    (void)end;
    if (view_scroll_offset == 0) {
      for (uint32_t x = cursor_x; x < max_cols; x++)
        draw_history_char(x, cursor_y);
    }
    break;
  }

  case '@': { // Insert N blank characters (ICH)
    int n = (value > 0) ? value : 1;
    uint32_t row = console_history_row(cursor_y);
    for (uint32_t x = max_cols - 1; x >= cursor_x + (uint32_t)n; x--) {
      history[row][x] = history[row][x - (uint32_t)n];
      if (x == cursor_x)
        break;
    }
    for (uint32_t x = cursor_x; x < cursor_x + (uint32_t)n && x < max_cols;
         x++) {
      history[row][x].c = 0;
      history[row][x].fg = current_fg;
      history[row][x].bg = current_bg;
    }
    if (view_scroll_offset == 0) {
      for (uint32_t x = cursor_x; x < max_cols; x++)
        draw_history_char(x, cursor_y);
    }
    break;
  }

  default:
    break;
  }
}

// Character drawing helpers

static void draw_char_colored(uint32_t c, uint32_t col, uint32_t row,
                              uint32_t fg, uint32_t bg, bool underline) {
  const uint8_t *glyph = font_get_glyph(c);
  uint32_t px = col * FONT_WIDTH;
  uint32_t py = row * FONT_HEIGHT;

  for (uint32_t y = 0; y < FONT_HEIGHT; y++) {
    // Bottom two rows become the underline bar when underline is active
    uint8_t bits = (underline && y >= FONT_HEIGHT - 2) ? 0xFF : glyph[y];
    fb_draw_glyph_scanline(px, py + y, bits, fg, bg);
  }
}

static void draw_history_char(uint32_t col, uint32_t row) {
  uint32_t history_row = console_history_row(row);
  console_char_t *ch = &history[history_row][col];

  uint32_t px = col * FONT_WIDTH;
  uint32_t py = row * FONT_HEIGHT;

  if (ch->c == 0) {
    fb_fill_rect(px, py, FONT_WIDTH, FONT_HEIGHT, ch->bg);
    return;
  }

  draw_char_colored(ch->c, col, row, ch->fg, ch->bg, ch->underline);
}

static void draw_cursor_block(uint32_t col, uint32_t row) {
  if (col >= max_cols || row >= max_rows)
    return;

  console_char_t *ch = &history[console_history_row(row)][col];
  uint32_t cell_fg = ch->fg ? ch->fg : FG_COLOR;
  uint32_t cp = ch->c ? ch->c : ' ';

  /* A block cursor is the underlying cell rendered in reverse video. */
  draw_char_colored(cp, col, row, ch->bg, cell_fg, false);
}

// Render a single decoded codepoint on the framebuffer console
static void console_render_char(uint32_t cp) {
  if (cp == '\n') {
    cursor_x = 0;

    // If the cursor was explicitly repositioned (e.g. ESC[H for a full-screen
    // app like kilo), just advance cursor_y without touching history_write_row.
    // This prevents the history mapping from drifting on each redraw frame.
    if (cursor_repositioned) {
      cursor_y++;
      if (cursor_y >= max_rows) {
        // Cursor overflowed the screen — need a real scroll.
        cursor_repositioned = false;
        if (view_scroll_offset == 0) {
          history_write_row++;
          scroll_up();
          uint32_t new_row = history_write_row % HISTORY_MAX;
          for (uint32_t i = 0; i < COLS_MAX; i++) {
            history[new_row][i].c = 0;
            history[new_row][i].fg = current_fg;
            history[new_row][i].bg = current_bg;
          }
        } else {
          cursor_y--;
          view_scroll_offset++;
          if (view_scroll_offset >= HISTORY_MAX - max_rows)
            view_scroll_offset = HISTORY_MAX - max_rows;
        }
      }
      return;
    }

    // Normal sequential output — advance history tracking.
    uint32_t bottom_screen =
        (history_write_row >= max_rows - 1) ? max_rows - 1 : history_write_row;
    bool at_bottom = (cursor_y >= bottom_screen);

    cursor_y++;

    if (at_bottom) {
      history_write_row++;
      uint32_t row = console_history_row(cursor_y);
      for (uint32_t i = 0; i < COLS_MAX; i++) {
        history[row][i].c = 0;
        history[row][i].fg = current_fg;
        history[row][i].bg = current_bg;
      }

      if (view_scroll_offset == 0 && cursor_y < max_rows) {
        fb_fill_rect(0, cursor_y * FONT_HEIGHT, fb_get_width(), FONT_HEIGHT,
                     current_bg);
      }
    }

    if (cursor_y >= max_rows) {
      if (view_scroll_offset == 0) {
        scroll_up();
        uint32_t new_row = history_write_row % HISTORY_MAX;
        for (uint32_t i = 0; i < COLS_MAX; i++) {
          history[new_row][i].c = 0;
          history[new_row][i].fg = current_fg;
          history[new_row][i].bg = current_bg;
        }
      } else {
        cursor_y--;
        view_scroll_offset++;
        if (view_scroll_offset >= HISTORY_MAX - max_rows)
          view_scroll_offset = HISTORY_MAX - max_rows;
      }
    }
    return;
  }

  if (cp == '\r') {
    cursor_x = 0;
    wrap_pending = false; // \r cancels any pending autowrap
    return;
  }

  if (cp == '\b') {
    if (cursor_x > 0) {
      cursor_x--;
    } else if (cursor_y > 0) {
      cursor_y--;
      cursor_x = max_cols - 1;
    }

    uint32_t row = console_history_row(cursor_y);
    history[row][cursor_x].c = ' ';
    history[row][cursor_x].fg = FG_COLOR;
    history[row][cursor_x].bg = BG_COLOR;

    if (view_scroll_offset == 0) {
      fb_fill_rect(cursor_x * FONT_WIDTH, cursor_y * FONT_HEIGHT, FONT_WIDTH,
                   FONT_HEIGHT, BG_COLOR);
    }
    return;
  }

  if (cp == '\t') {
    for (int i = 0; i < 4; i++)
      console_render_char(' ');
    return;
  }

  // Deferred autowrap: if a previous character hit the right margin,
  // perform the actual line-feed now (before drawing this new character).
  if (wrap_pending) {
    wrap_pending = false;
    cursor_x = 0;
    // Reuse the newline logic for advancing the row
    console_render_char('\n');
  }

  // Store codepoint with current SGR colors
  if (cursor_x < COLS_MAX) {
    uint32_t row = console_history_row(cursor_y);
    history[row][cursor_x].c = cp;
    history[row][cursor_x].fg = current_fg;
    history[row][cursor_x].bg = current_bg;
    history[row][cursor_x].bold = attr_bold;
    history[row][cursor_x].underline = attr_underline;
  }

  if (view_scroll_offset == 0) {
    draw_char_colored(cp, cursor_x, cursor_y, current_fg, current_bg,
                      attr_underline);
  }

  last_printed_cp = cp; // track for REP (ESC[nb)
  cursor_x++;
  if (cursor_x >= max_cols) {
    // Don't wrap immediately — defer until the next printable character.
    // This matches real terminal behavior ("autowrap pending").
    // A \r or cursor-positioning escape will cancel the pending wrap.
    wrap_pending = true;
    cursor_x = max_cols - 1; // cursor stays at last column
  }
}

// Core putchar with UTF-8 decoding (must be called with console_lock held) ─
static void console_putchar_unlocked(char c) {
  unsigned char uc = (unsigned char)c;

  // OSC strings end with BEL or ST (ESC followed by '\\'). Unsupported OSC
  // commands are deliberately ignored, matching terminals which do not
  // implement hyperlinks, title changes, clipboard access, etc.
  if (terminal_osc) {
    if (uc == 0x07 || uc == 0x9C) {
      terminal_osc = false;
      terminal_osc_esc = false;
      return;
    }
    if (terminal_osc_esc) {
      if (c == '\\') {
        terminal_osc = false;
        terminal_osc_esc = false;
        return;
      }
      terminal_osc_esc = (uc == 0x1B);
      return;
    }
    if (uc == 0x1B)
      terminal_osc_esc = true;
    return;
  }

  // ANSI escape sequences (all ASCII, no UTF-8 conflict)
  if (terminal_escape) {
    if (terminal_escape_len < sizeof(terminal_escape_buffer) - 1) {
      terminal_escape_buffer[terminal_escape_len++] = c;
    }

    // Two-character escape sequences: ESC followed by a single final byte
    // from 0x40–0x7E (but not '[' which starts CSI, and not intermediaries).
    // Intermediary bytes are 0x20–0x2F; they prefix a final byte.
    // We terminate on the first byte in 0x40–0x7E that follows 0+
    // intermediaries, OR on any byte in 0x40–0x7E that is not '[' when it's the
    // first byte.
    if (terminal_escape_len == 1) {
      // First byte after ESC
      if (c == ']') {
        terminal_escape = false;
        terminal_escape_len = 0;
        terminal_osc = true;
        terminal_osc_esc = false;
        return;
      }
      if (c == '[' || c == 'O') {
        // CSI or SS3 — accumulate until final byte
        return;
      }
      // Single-byte intermediaries (0x20–0x2F): accumulate another byte
      if (c >= 0x20 && c <= 0x2F) {
        return; // wait for the final byte
      }
      // Final byte of a 2-char sequence (0x40–0x7E) or special bytes
      if (c >= 0x40 && c <= 0x7E) {
        // Handle specific 2-char ESC sequences
        switch (c) {
        case 'M': { // Reverse Index (RI) — scroll down / cursor up
          if (cursor_y > scroll_region_top) {
            cursor_y--;
          } else {
            // At top of scroll region: scroll region down one line
            // Shift rows in scroll region down
            uint32_t bot = scroll_bottom();
            for (uint32_t row = bot; row > scroll_region_top; row--) {
              uint32_t dst = console_history_row(row);
              uint32_t src = console_history_row(row - 1);
              for (uint32_t x = 0; x < COLS_MAX; x++)
                history[dst][x] = history[src][x];
            }
            // Clear the top line
            uint32_t top_row = console_history_row(scroll_region_top);
            for (uint32_t x = 0; x < COLS_MAX; x++) {
              history[top_row][x].c = 0;
              history[top_row][x].fg = FG_COLOR;
              history[top_row][x].bg = BG_COLOR;
            }
            console_redraw();
          }
          break;
        }
        case '7': // Save cursor
          saved_cursor_x = cursor_x;
          saved_cursor_y = cursor_y;
          break;
        case '8': // Restore cursor
          cursor_x = saved_cursor_x;
          cursor_y = saved_cursor_y;
          if (cursor_x >= max_cols)
            cursor_x = max_cols - 1;
          if (cursor_y >= max_rows)
            cursor_y = max_rows - 1;
          wrap_pending = false;
          break;
        case '=': // Keypad application mode — no-op
        case '>': // Keypad normal mode — no-op
        case 'c': // RIS — full reset, treat as clear
          console_wipe_history_unlocked();
          break;
        default:
          break;
        }
        terminal_escape = false;
        terminal_escape_len = 0;
        return;
      }
      // Non-final, non-intermediate byte (e.g. 0x30–0x3F numeric) - keep
      // accumulating (shouldn't happen in well-formed VT but be safe)
      return;
    }

    // Second byte after ESC + one intermediary (e.g. ESC ( 0 or ESC ) B)
    if (terminal_escape_len == 2 && terminal_escape_buffer[0] >= 0x20 &&
        terminal_escape_buffer[0] <= 0x2F) {
      // This is the final byte of a 3-char sequence like ESC ( 0
      char inter = terminal_escape_buffer[0];
      char fin = c;
      // ESC ( 0 — designate G1 as VT100 line drawing (we track but render via
      // acs_active) ESC ( B — designate G0/G1 as ASCII (no-op for us, just
      // reset flag if needed) We don't differentiate G0/G1 designation
      // internals beyond the SO/SI switching, so just silently accept all of
      // them.
      (void)inter;
      (void)fin;
      terminal_escape = false;
      terminal_escape_len = 0;
      return;
    }

    // CSI sequence (starts with '['): terminate on final byte 0x40–0x7E
    if (terminal_escape_buffer[0] == '[') {
      if (c >= '@' && c <= '~') {
        console_process_escape_sequence();
        terminal_escape = false;
        terminal_escape_len = 0;
      }
      return;
    }

    // SS3 sequence (starts with 'O'): terminate on final byte 0x40–0x7E
    if (terminal_escape_buffer[0] == 'O') {
      if (c >= '@' && c <= '~') {
        // SS3 sequences are usually single-character commands like OP, OQ.
        // We don't have any specific console-side handlers for SS3 input echo
        // other than ignoring them as a full sequence instead of rendering them.
        terminal_escape = false;
        terminal_escape_len = 0;
      }
      return;
    }

    // Safety: if we've accumulated too many bytes without termination, flush
    terminal_escape = false;
    terminal_escape_len = 0;
    return;
  }

  if (c == 0x1B) {
    terminal_escape = true;
    terminal_escape_len = 0;
    return;
  }

  // SO (0x0E) — switch to G1 character set (line drawing)
  if (uc == 0x0E) {
    acs_active = true;
    return;
  }
  // SI (0x0F) — switch back to G0 character set (ASCII)
  if (uc == 0x0F) {
    acs_active = false;
    return;
  }

  // Always forward raw bytes to serial (serial terminals handle UTF-8 natively)
  serial_putchar(c);

  // UTF-8 multi-byte decoding
  // Continuation byte (10xxxxxx)
  if ((uc & 0xC0) == 0x80) {
    if (utf8_bytes_remaining > 0) {
      utf8_codepoint = (utf8_codepoint << 6) | (uc & 0x3F);
      utf8_bytes_remaining--;
      if (utf8_bytes_remaining == 0) {
        console_render_char(utf8_codepoint);
      }
    }
    // Stray continuation bytes are silently dropped
    return;
  }

  // If we were mid-sequence, the sequence was broken — reset
  utf8_bytes_remaining = 0;

  // 2-byte lead (110xxxxx)
  if (uc >= 0xC0 && uc < 0xE0) {
    utf8_codepoint = uc & 0x1F;
    utf8_bytes_remaining = 1;
    return;
  }
  // 3-byte lead (1110xxxx)
  if (uc >= 0xE0 && uc < 0xF0) {
    utf8_codepoint = uc & 0x0F;
    utf8_bytes_remaining = 2;
    return;
  }
  // 4-byte lead (11110xxx)
  if (uc >= 0xF0 && uc < 0xF8) {
    utf8_codepoint = uc & 0x07;
    utf8_bytes_remaining = 3;
    return;
  }

  // Plain ASCII byte — check for ACS (line drawing) mode first
  if (acs_active && uc >= 0x60 && uc <= 0x7E) {
    console_render_char(acs_map[uc - 0x60]);
    return;
  }
  console_render_char((uint32_t)uc);
}

// Public API

void console_putchar(char c) {
  if (fb_get_kd_mode() == KD_GRAPHICS)
    return;
  spinlock_acquire(&console_lock);

  // Use backbuffer even for single putchar to keep it in sync with puts
  // and prevent stale backbuffer data from overwriting frontbuffer during
  // swaps.
  fb_set_backbuffer_mode(true);
  console_putchar_unlocked(c);
  fb_swap_buffer();
  fb_set_backbuffer_mode(false);

  spinlock_release(&console_lock);
}

// Batch-aware puts: writes all characters then does ONE framebuffer blit.
// This is the main anti-flicker fix — kilo sends one large write() per frame
// so everything lands in one swap.
void console_puts(const char *s) {
  spinlock_acquire(&console_lock);
  fb_set_backbuffer_mode(true);

  bool was_visible = cursor_logical_visible;
  if (was_visible && view_scroll_offset == 0)
    console_set_cursor_visible_unlocked(false);

  while (*s)
    console_putchar_unlocked(*s++);

  if (was_visible && view_scroll_offset == 0)
    console_set_cursor_visible_unlocked(true);

  fb_swap_buffer();
  fb_set_backbuffer_mode(false);
  spinlock_release(&console_lock);
}

// Batch write used by the VFS console node and fd 1/2 in sys_write.
// Renders all characters then swaps the backbuffer exactly once.
void console_write_batch(const char *buf, size_t len) {
  if (fb_get_kd_mode() == KD_GRAPHICS)
    return;
  spinlock_acquire(&console_lock);

  fb_set_backbuffer_mode(true);

  // Don't toggle cursor visibility - just skip drawing it and restore after
  bool cursor_was_on = cursor_phys_on;
  if ((cursor_logical_visible || cursor_was_on) &&
      view_scroll_offset == 0)
    draw_history_char(cursor_x, cursor_y);
  cursor_phys_on = false;

  for (size_t i = 0; i < len; i++)
    console_putchar_unlocked(buf[i]);

  // Keep an interactive cursor visible at its new position after output.
  cursor_phys_on = cursor_logical_visible;
  if (cursor_phys_on && view_scroll_offset == 0)
    draw_cursor_block(cursor_x, cursor_y);

  fb_swap_buffer();
  fb_set_backbuffer_mode(false);

  spinlock_release(&console_lock);
}

static void console_set_cursor_visible_unlocked(bool visible) {
  cursor_logical_visible = visible;
  if (visible) {
    cursor_phys_on = true;
    last_blink_ms = lapic_timer_get_ms();

    draw_cursor_block(cursor_x, cursor_y);
  } else {
    cursor_phys_on = false;
    draw_history_char(cursor_x, cursor_y);
  }
}

void console_set_cursor_visible(bool visible) {
  spinlock_acquire(&console_lock);
  console_set_cursor_visible_unlocked(visible);
  spinlock_release(&console_lock);
}

static void console_refresh_cursor_unlocked(void) {
  if (!cursor_logical_visible || view_scroll_offset != 0)
    return;

  uint64_t now = lapic_timer_get_ms();
  if (now - last_blink_ms < 500)
    return;

  last_blink_ms = now;
  cursor_phys_on = !cursor_phys_on;

  if (cursor_phys_on) {
    draw_cursor_block(cursor_x, cursor_y);
  } else {
    draw_history_char(cursor_x, cursor_y);
  }
}

void console_refresh_cursor(void) {
  if (fb_get_kd_mode() == KD_GRAPHICS)
    return;
  spinlock_acquire(&console_lock);
  console_refresh_cursor_unlocked();
  spinlock_release(&console_lock);
}

void console_redraw_all(void) {
  if (fb_get_kd_mode() == KD_GRAPHICS)
    return;
  spinlock_acquire(&console_lock);
  console_redraw();
  spinlock_release(&console_lock);
}

void console_clear(void) {
  fb_set_kd_mode(KD_TEXT);
  spinlock_acquire(&console_lock);
  console_wipe_history_unlocked();
  spinlock_release(&console_lock);
}
