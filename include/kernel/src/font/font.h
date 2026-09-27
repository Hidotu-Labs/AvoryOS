#ifndef FONT_FONT_H
#define FONT_FONT_H

#include <stdint.h>

#define FONT_WIDTH  8
#define FONT_HEIGHT 16

const uint8_t *font_get_glyph(uint32_t codepoint);

#endif
