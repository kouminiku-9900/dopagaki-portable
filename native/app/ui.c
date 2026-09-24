#include "ui.h"

#include <string.h>

#include "tilefinch/font.h"

#include "komi_runtime.h"

#define GLYPH_CACHE 512

typedef struct {
    unsigned codepoint;
    int pixel_height;
    bool valid;
    FontGlyph glyph;
} CachedGlyph;

/* What ui_* draws on: pixel (x, y) is surface[origin + x * step_x +
   y * step_y]. The landscape back buffer unless a program switched to
   another surface (the portrait canvas, or the back buffer through a
   rotation). */
static int surface_width = KOMI_SCREEN_WIDTH;
static int surface_height = KOMI_SCREEN_HEIGHT;
static long surface_step_x = 1;
static long surface_step_y = KOMI_VRAM_STRIDE;
static long surface_origin = 0;

void ui_set_surface(int width, int height, long step_x, long step_y,
                    long origin)
{
    surface_width = width;
    surface_height = height;
    surface_step_x = step_x;
    surface_step_y = step_y;
    surface_origin = origin;
}

static inline uint16_t *pixel_at(uint16_t *vram, int x, int y)
{
    return vram + surface_origin + (long) x * surface_step_x
        + (long) y * surface_step_y;
}

static FontFace face;
static bool face_loaded;
static CachedGlyph cache[GLYPH_CACHE];

bool ui_init(Budget *budget, const char *font_path)
{
    FontFaceLoad *load = font_face_load_begin(budget, font_path, 512u * 1024u);
    if (load == NULL) return false;
    FontFaceLoadStatus status = FONT_FACE_LOAD_PENDING;
    while (status == FONT_FACE_LOAD_PENDING)
        status = font_face_load_pump(load, 64u * 1024u, &face);
    font_face_load_destroy(load);
    face_loaded = status == FONT_FACE_LOAD_COMPLETE;
    return face_loaded;
}

static const FontGlyph *glyph_for(unsigned codepoint, int pixel_height)
{
    if (!face_loaded) return NULL;
    size_t slot = (codepoint * 2654435761u + (unsigned) pixel_height * 97u)
        % GLYPH_CACHE;
    CachedGlyph *entry = &cache[slot];
    if (entry->valid && entry->codepoint == codepoint
        && entry->pixel_height == pixel_height)
        return &entry->glyph;
    if (entry->valid) font_glyph_destroy(&face, &entry->glyph);
    memset(entry, 0, sizeof *entry);
    if (!font_glyph_load(&face, codepoint, pixel_height, false,
                         &entry->glyph))
        return NULL;
    entry->valid = true;
    entry->codepoint = codepoint;
    entry->pixel_height = pixel_height;
    return &entry->glyph;
}

/* Clip a box to the surface; false when nothing is left. */
static bool clip(int *x, int *y, int *width, int *height)
{
    if (*x < 0) { *width += *x; *x = 0; }
    if (*y < 0) { *height += *y; *y = 0; }
    if (*x + *width > surface_width) *width = surface_width - *x;
    if (*y + *height > surface_height) *height = surface_height - *y;
    return *width > 0 && *height > 0;
}

/* Visit a clipped box along whichever axis is contiguous in memory. */
#define FOR_BOX(vram, x, y, width, height, p, body)                         \
    do {                                                                    \
        bool rows_contiguous = surface_step_x == 1 || surface_step_x == -1; \
        int outer = rows_contiguous ? (height) : (width);                   \
        int inner = rows_contiguous ? (width) : (height);                   \
        long inner_step = rows_contiguous ? surface_step_x : surface_step_y;\
        for (int o = 0; o < outer; o++) {                                   \
            uint16_t *p = rows_contiguous                                   \
                ? pixel_at(vram, (x), (y) + o)                              \
                : pixel_at(vram, (x) + o, (y));                             \
            for (int i = 0; i < inner; i++, p += inner_step) { body; }      \
        }                                                                   \
    } while (0)

void ui_fill(uint16_t *vram, int x, int y, int width, int height,
             uint16_t color)
{
    if (!clip(&x, &y, &width, &height)) return;
    FOR_BOX(vram, x, y, width, height, p, *p = color);
}

void ui_shade(uint16_t *vram, int x, int y, int width, int height)
{
    if (!clip(&x, &y, &width, &height)) return;
    FOR_BOX(vram, x, y, width, height, p,
            *p = (uint16_t) ((*p >> 1) & 0x7BEFu));
}

static uint16_t blend(uint16_t over, uint16_t under, unsigned alpha)
{
    unsigned r = ((over & 31u) * alpha + (under & 31u) * (255u - alpha)) / 255u;
    unsigned g = (((over >> 5) & 63u) * alpha
                  + ((under >> 5) & 63u) * (255u - alpha)) / 255u;
    unsigned b = (((over >> 11) & 31u) * alpha
                  + ((under >> 11) & 31u) * (255u - alpha)) / 255u;
    return (uint16_t) (r | (g << 5) | (b << 11));
}

static void draw_glyph(uint16_t *vram, int x, int baseline,
                       const FontGlyph *glyph, uint16_t color)
{
    if (glyph->pixels == NULL) return;
    int left = x + glyph->x_offset;
    int top = baseline + glyph->y_offset;
    for (int row = 0; row < glyph->height; row++) {
        int y = top + row;
        if (y < 0 || y >= surface_height) continue;
        const unsigned char *coverage =
            glyph->pixels + (size_t) row * (size_t) glyph->width;
        for (int column = 0; column < glyph->width; column++) {
            int px = left + column;
            unsigned alpha = coverage[column];
            if (alpha == 0 || px < 0 || px >= surface_width) continue;
            uint16_t *p = pixel_at(vram, px, y);
            *p = alpha >= 250u ? color : blend(color, *p, alpha);
        }
    }
}

/* --- icons --- */

static void plot(uint16_t *vram, int x, int y, uint16_t color)
{
    if (x >= 0 && x < surface_width && y >= 0 && y < surface_height)
        *pixel_at(vram, x, y) = color;
}

/* size x size box with its top-left at (x, y). */
static void draw_icon(uint16_t *vram, int x, int y, int size, unsigned icon,
                      uint16_t text_color)
{
    int s = size;
    int c2 = s - 1; /* centre, doubled */
    switch (icon) {
    case 0xE000: { /* circle: red ring */
        uint16_t color = UI_RGB(240, 90, 90);
        int r2o = (s / 2) * (s / 2), r2i = (s / 2 - 2) * (s / 2 - 2);
        for (int j = 0; j < s; j++)
            for (int i = 0; i < s; i++) {
                int dx = 2 * i - c2, dy = 2 * j - c2;
                int d = (dx * dx + dy * dy) / 4;
                if (d <= r2o && d >= r2i) plot(vram, x + i, y + j, color);
            }
        break;
    }
    case 0xE001: { /* cross: blue X */
        uint16_t color = UI_RGB(110, 150, 250);
        for (int i = 1; i < s - 1; i++) {
            plot(vram, x + i, y + i, color);
            plot(vram, x + i + 1, y + i, color);
            plot(vram, x + s - 1 - i, y + i, color);
            plot(vram, x + s - 2 - i, y + i, color);
        }
        break;
    }
    case 0xE002: { /* triangle: green outline */
        uint16_t color = UI_RGB(80, 210, 150);
        for (int j = 1; j < s - 1; j++) {
            int half = (j * (s - 1)) / (2 * (s - 2));
            int mid = (s - 1) / 2;
            plot(vram, x + mid - half, y + j, color);
            plot(vram, x + mid + half, y + j, color);
            if (j == s - 2)
                for (int i = mid - half; i <= mid + half; i++)
                    plot(vram, x + i, y + j, color);
        }
        break;
    }
    case 0xE003: { /* square: pink outline */
        uint16_t color = UI_RGB(240, 120, 200);
        for (int i = 1; i < s - 1; i++) {
            plot(vram, x + i, y + 1, color);
            plot(vram, x + i, y + s - 2, color);
            plot(vram, x + 1, y + i, color);
            plot(vram, x + s - 2, y + i, color);
        }
        break;
    }
    case 0xE004: /* play: filled triangle pointing right */
        for (int j = 1; j < s - 1; j++) {
            int reach = j < s / 2 ? j : s - 1 - j;
            for (int i = 0; i <= reach * 2 && i < s - 2; i++)
                plot(vram, x + 2 + i, y + j, text_color);
        }
        break;
    case 0xE005: /* pause: two bars */
        for (int j = 1; j < s - 1; j++)
            for (int i = 0; i < s / 4; i++) {
                plot(vram, x + s / 5 + i, y + j, text_color);
                plot(vram, x + s - s / 5 - s / 4 + i, y + j, text_color);
            }
        break;
    default:
        break;
    }
}

static bool is_icon(unsigned codepoint)
{
    return codepoint >= 0xE000u && codepoint <= 0xE005u;
}

static int advance_of(const FontGlyph *glyph, int pixel_height)
{
    if (glyph == NULL) return pixel_height / 2;
    return glyph->advance > 0 ? glyph->advance : 0;
}

int ui_text_width(const char *text, int pixel_height)
{
    int width = 0;
    size_t length = strlen(text);
    size_t at = 0;
    while (at < length) {
        unsigned codepoint = 0;
        size_t used = font_utf8_next(text + at, length - at, &codepoint);
        if (used == 0) break;
        at += used;
        width += is_icon(codepoint) ? pixel_height + 2
            : advance_of(glyph_for(codepoint, pixel_height), pixel_height);
    }
    return width;
}

int ui_text(uint16_t *vram, int x, int y, int max_x, const char *text,
            int pixel_height, uint16_t color)
{
    size_t length = strlen(text);
    int baseline = y + pixel_height * 13 / 16;
    const FontGlyph *dot = glyph_for('.', pixel_height);
    int ellipsis = 3 * advance_of(dot, pixel_height);
    bool fits = x + ui_text_width(text, pixel_height) <= max_x;
    int limit = fits ? max_x : max_x - ellipsis;
    size_t at = 0;
    while (at < length) {
        unsigned codepoint = 0;
        size_t used = font_utf8_next(text + at, length - at, &codepoint);
        if (used == 0) break;
        if (is_icon(codepoint)) {
            if (x + pixel_height + 2 > limit) break;
            draw_icon(vram, x, y, pixel_height, codepoint, color);
            x += pixel_height + 2;
            at += used;
            continue;
        }
        const FontGlyph *glyph = glyph_for(codepoint, pixel_height);
        int advance = advance_of(glyph, pixel_height);
        if (x + advance > limit) break;
        if (glyph != NULL) draw_glyph(vram, x, baseline, glyph, color);
        x += advance;
        at += used;
    }
    if (!fits && dot != NULL) {
        for (int i = 0; i < 3; i++) {
            draw_glyph(vram, x, baseline, dot, color);
            x += advance_of(dot, pixel_height);
        }
    }
    return x;
}

int ui_text_wrap(uint16_t *vram, int x, int y, int max_x, const char *text,
                 int pixel_height, uint16_t color, int line_height,
                 int max_lines)
{
    size_t length = strlen(text);
    size_t at = 0;
    int line = 0;
    while (at < length && line < max_lines) {
        /* The longest prefix that fits; the last line gets the ellipsis. */
        size_t end = at;
        int width = 0;
        while (end < length) {
            unsigned codepoint = 0;
            size_t used = font_utf8_next(text + end, length - end, &codepoint);
            if (used == 0) break;
            int advance = is_icon(codepoint) ? pixel_height + 2
                : advance_of(glyph_for(codepoint, pixel_height),
                             pixel_height);
            if (x + width + advance > max_x) break;
            width += advance;
            end += used;
        }
        if (end == at) break;
        char part[512];
        bool last = line == max_lines - 1;
        size_t take = last ? length - at : end - at;
        if (take >= sizeof part) take = sizeof part - 1;
        memcpy(part, text + at, take);
        part[take] = '\0';
        ui_text(vram, x, y + line * line_height, max_x, part, pixel_height,
                color);
        at = end;
        line++;
    }
    return line;
}
