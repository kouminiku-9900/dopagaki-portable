/* Drawing for the native screens: filled boxes and UTF-8 text on the
 * 16-bit (RGB565, 512-pixel stride) page surface.
 *
 * Text goes through tilefinch's font code: Latin from TilefinchSans, and
 * Japanese from the Unifont CJK bitmaps compiled into tilefinch_core, which
 * font_glyph_load falls back to for code points the face lacks. Glyphs are
 * cached per (code point, size). */
#ifndef KOMI_UI_H
#define KOMI_UI_H

#include <stdbool.h>
#include <stdint.h>

#include "tilefinch/budget.h"

#define UI_RGB(r, g, b) \
    ((uint16_t) ((((b) >> 3) << 11) | (((g) >> 2) << 5) | ((r) >> 3)))

#define UI_BACKGROUND UI_RGB(18, 18, 22)
#define UI_BAR UI_RGB(34, 34, 40)
#define UI_ROW_SELECTED UI_RGB(52, 60, 86)
#define UI_TEXT UI_RGB(236, 236, 240)
#define UI_TEXT_MUTED UI_RGB(150, 150, 160)
#define UI_ACCENT UI_RGB(230, 60, 60)
#define UI_LIKED UI_RGB(255, 110, 150)

/* Icons drawn in place of these private-use code points inside ui_text
   strings (the fonts have no △, ▶ or Ⅱ). The PSP face-button colours. */
#define UI_ICON_CIRCLE "\uE000"
#define UI_ICON_CROSS "\uE001"
#define UI_ICON_TRIANGLE "\uE002"
#define UI_ICON_SQUARE "\uE003"
#define UI_ICON_PLAY "\uE004"
#define UI_ICON_PAUSE "\uE005"

/* Load the Latin face from font_path. Without it, Latin falls back to the
   built-in glyphs and Japanese still works. */
bool ui_init(Budget *budget, const char *font_path);

/* The surface the functions below draw on (default: the landscape back
   buffer, KOMI_SCREEN_* at KOMI_VRAM_STRIDE). */
void ui_set_surface(int width, int height, int stride);

void ui_fill(uint16_t *vram, int x, int y, int width, int height,
             uint16_t color);
/* Darken a box to half brightness (overlay backgrounds over video). */
void ui_shade(uint16_t *vram, int x, int y, int width, int height);

/* Draw text with its top at y, clipped at max_x with "..." when it does not
   fit. Returns the x after the last drawn glyph. */
int ui_text(uint16_t *vram, int x, int y, int max_x, const char *text,
            int pixel_height, uint16_t color);
int ui_text_width(const char *text, int pixel_height);
/* Text broken into lines of line_height, at most max_lines (the last one
   ends in "..." when the text does not fit). Returns the lines drawn. */
int ui_text_wrap(uint16_t *vram, int x, int y, int max_x, const char *text,
                 int pixel_height, uint16_t color, int line_height,
                 int max_lines);

#endif
