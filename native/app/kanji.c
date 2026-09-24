#include "kanji.h"

#include <stdio.h>
#include <string.h>

/* A tiny JSON reader: just enough to walk nested arrays of strings. */
typedef struct {
    const char *at;
    const char *end;
} Reader;

static void skip_space(Reader *r)
{
    while (r->at < r->end
           && (*r->at == ' ' || *r->at == '\n' || *r->at == '\r'
               || *r->at == '\t'))
        r->at++;
}

static bool expect(Reader *r, char c)
{
    skip_space(r);
    if (r->at >= r->end || *r->at != c) return false;
    r->at++;
    return true;
}

static bool peek(Reader *r, char c)
{
    skip_space(r);
    return r->at < r->end && *r->at == c;
}

static size_t put_utf8(char *out, size_t room, unsigned cp)
{
    char b[4];
    size_t n;
    if (cp < 0x80u) { b[0] = (char) cp; n = 1; }
    else if (cp < 0x800u) {
        b[0] = (char) (0xC0u | (cp >> 6));
        b[1] = (char) (0x80u | (cp & 0x3Fu)); n = 2;
    } else if (cp < 0x10000u) {
        b[0] = (char) (0xE0u | (cp >> 12));
        b[1] = (char) (0x80u | ((cp >> 6) & 0x3Fu));
        b[2] = (char) (0x80u | (cp & 0x3Fu)); n = 3;
    } else {
        b[0] = (char) (0xF0u | (cp >> 18));
        b[1] = (char) (0x80u | ((cp >> 12) & 0x3Fu));
        b[2] = (char) (0x80u | ((cp >> 6) & 0x3Fu));
        b[3] = (char) (0x80u | (cp & 0x3Fu)); n = 4;
    }
    if (n > room) return 0;
    memcpy(out, b, n);
    return n;
}

static bool hex4(const char *at, unsigned *value)
{
    unsigned v = 0;
    for (int i = 0; i < 4; i++) {
        char c = at[i];
        v <<= 4;
        if (c >= '0' && c <= '9') v |= (unsigned) (c - '0');
        else if (c >= 'a' && c <= 'f') v |= (unsigned) (c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') v |= (unsigned) (c - 'A' + 10);
        else return false;
    }
    *value = v;
    return true;
}

/* Read a string into out (NULL to skip). Truncates at a UTF-8 boundary. */
static bool read_string(Reader *r, char *out, size_t size)
{
    if (!expect(r, '"')) return false;
    size_t used = 0;
    bool full = false;
    while (r->at < r->end && *r->at != '"') {
        unsigned cp;
        size_t raw = 1;
        if (*r->at == '\\') {
            if (r->end - r->at < 2) return false;
            char e = r->at[1];
            raw = 2;
            if (e == 'u') {
                if (r->end - r->at < 6 || !hex4(r->at + 2, &cp)) return false;
                raw = 6;
                if (cp >= 0xD800u && cp <= 0xDBFFu && r->end - r->at >= 12
                    && r->at[6] == '\\' && r->at[7] == 'u') {
                    unsigned low;
                    if (hex4(r->at + 8, &low) && low >= 0xDC00u
                        && low <= 0xDFFFu) {
                        cp = 0x10000u + ((cp - 0xD800u) << 10)
                            + (low - 0xDC00u);
                        raw = 12;
                    }
                }
            } else {
                cp = e == 'n' ? '\n' : e == 't' ? '\t' : e == 'r' ? '\r'
                    : e == 'b' ? '\b' : e == 'f' ? '\f' : (unsigned char) e;
            }
            if (out != NULL && !full) {
                size_t n = put_utf8(out + used, size - 1 - used, cp);
                if (n == 0) full = true;
                used += n;
            }
            r->at += raw;
            continue;
        }
        unsigned char lead = (unsigned char) *r->at;
        size_t n = lead < 0x80u ? 1 : lead >= 0xF0u ? 4 : lead >= 0xE0u ? 3
            : lead >= 0xC0u ? 2 : 1;
        if ((size_t) (r->end - r->at) < n) return false;
        if (out != NULL && !full) {
            if (used + n < size) {
                memcpy(out + used, r->at, n);
                used += n;
            } else {
                full = true;
            }
        }
        r->at += n;
    }
    if (out != NULL) out[used] = '\0';
    return expect(r, '"');
}

/* Skip any JSON value. */
static bool skip_value(Reader *r)
{
    skip_space(r);
    if (r->at >= r->end) return false;
    char c = *r->at;
    if (c == '"') return read_string(r, NULL, 0);
    if (c == '[' || c == '{') {
        char close = c == '[' ? ']' : '}';
        r->at++;
        if (peek(r, close)) { r->at++; return true; }
        for (;;) {
            if (c == '{') {
                if (!read_string(r, NULL, 0) || !expect(r, ':')) return false;
            }
            if (!skip_value(r)) return false;
            if (peek(r, ',')) { r->at++; continue; }
            return expect(r, close);
        }
    }
    while (r->at < r->end && *r->at != ',' && *r->at != ']'
           && *r->at != '}')
        r->at++;
    return true;
}

bool kanji_parse_transliteration(const char *json, size_t length,
                                 KanjiConversion *conversion)
{
    memset(conversion, 0, sizeof *conversion);
    Reader r = {json, json + length};
    if (!expect(&r, '[')) return false;
    while (!peek(&r, ']')) {
        if (!expect(&r, '[')) return false;
        KanjiSegment scratch;
        KanjiSegment *segment = conversion->count < KANJI_SEGMENTS
            ? &conversion->segments[conversion->count] : &scratch;
        memset(segment, 0, sizeof *segment);
        if (!read_string(&r, segment->reading, sizeof segment->reading)
            || !expect(&r, ',') || !expect(&r, '['))
            return false;
        while (!peek(&r, ']')) {
            char *slot = segment->count < KANJI_CANDIDATES
                ? segment->candidates[segment->count] : NULL;
            if (!read_string(&r, slot, KANJI_TEXT)) return false;
            if (slot != NULL) segment->count++;
            if (peek(&r, ',')) r.at++;
        }
        r.at++; /* ] */
        while (peek(&r, ',')) { r.at++; if (!peek(&r, ']')) skip_value(&r); }
        if (!expect(&r, ']')) return false;
        if (segment != &scratch) conversion->count++;
        if (peek(&r, ',')) r.at++;
    }
    return conversion->count > 0;
}

size_t kanji_parse_suggestions(const char *json, size_t length,
                               char out[][KANJI_TEXT], size_t limit)
{
    Reader r = {json, json + length};
    size_t count = 0;
    if (!expect(&r, '[') || !read_string(&r, NULL, 0) || !expect(&r, ',')
        || !expect(&r, '['))
        return 0;
    while (!peek(&r, ']')) {
        if (peek(&r, '"')) {
            char *slot = count < limit ? out[count] : NULL;
            if (!read_string(&r, slot, KANJI_TEXT)) break;
            if (slot != NULL) count++;
        } else if (!skip_value(&r)) {
            break;
        }
        if (peek(&r, ',')) r.at++;
    }
    return count;
}

bool kanji_has_hiragana(const char *text)
{
    const unsigned char *at = (const unsigned char *) text;
    for (; *at != '\0'; at++) {
        /* U+3041..U+3096 is E3 81 81 .. E3 82 96 */
        if (at[0] == 0xE3 && at[1] != '\0' && at[2] != '\0') {
            unsigned cp = ((at[0] & 0x0Fu) << 12) | ((at[1] & 0x3Fu) << 6)
                | (at[2] & 0x3Fu);
            if (cp >= 0x3041u && cp <= 0x3096u) return true;
        }
    }
    return false;
}

bool kanji_url_encode(const char *text, char *out, size_t size)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t used = 0;
    for (const unsigned char *at = (const unsigned char *) text; *at; at++) {
        unsigned char c = *at;
        bool plain = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
            || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.'
            || c == '~';
        if (plain) {
            if (used + 1 >= size) return false;
            out[used++] = (char) c;
        } else {
            if (used + 3 >= size) return false;
            out[used++] = '%';
            out[used++] = hex[c >> 4];
            out[used++] = hex[c & 15u];
        }
    }
    out[used] = '\0';
    return true;
}
