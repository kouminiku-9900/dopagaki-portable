#include "yt_results.h"

#include <stdio.h>
#include <string.h>

/* Bounded strstr over [at, end). */
static const char *find(const char *at, const char *end, const char *needle)
{
    size_t length = strlen(needle);
    for (; at != NULL && at + length <= end; at++) {
        if (*at == *needle && memcmp(at, needle, length) == 0) return at;
    }
    return NULL;
}

/* Append one Unicode code point as UTF-8. */
static size_t put_utf8(char *out, size_t room, unsigned codepoint)
{
    char bytes[4];
    size_t count;
    if (codepoint < 0x80u) {
        bytes[0] = (char) codepoint;
        count = 1;
    } else if (codepoint < 0x800u) {
        bytes[0] = (char) (0xC0u | (codepoint >> 6));
        bytes[1] = (char) (0x80u | (codepoint & 0x3Fu));
        count = 2;
    } else if (codepoint < 0x10000u) {
        bytes[0] = (char) (0xE0u | (codepoint >> 12));
        bytes[1] = (char) (0x80u | ((codepoint >> 6) & 0x3Fu));
        bytes[2] = (char) (0x80u | (codepoint & 0x3Fu));
        count = 3;
    } else {
        bytes[0] = (char) (0xF0u | (codepoint >> 18));
        bytes[1] = (char) (0x80u | ((codepoint >> 12) & 0x3Fu));
        bytes[2] = (char) (0x80u | ((codepoint >> 6) & 0x3Fu));
        bytes[3] = (char) (0x80u | (codepoint & 0x3Fu));
        count = 4;
    }
    if (count > room) return 0;
    memcpy(out, bytes, count);
    return count;
}

/* Copy HTML text [at, end) into out, decoding the entities the writer
   emits (lite_html_escape) plus numeric references, and dropping tags.
   Never splits a UTF-8 sequence when the output is full. */
static void copy_text(const char *at, const char *end, char *out, size_t size)
{
    size_t used = 0;
    if (size == 0) return;
    while (at < end && used + 1 < size) {
        if (*at == '<') {
            const char *close = find(at, end, ">");
            at = close == NULL ? end : close + 1;
            continue;
        }
        if (*at == '&') {
            static const struct { const char *name; unsigned codepoint; }
                named[] = {
                    {"&amp;", '&'}, {"&lt;", '<'}, {"&gt;", '>'},
                    {"&quot;", '"'}, {"&#39;", '\''}, {"&apos;", '\''},
                    {"&middot;", 0xB7u}, {"&nbsp;", ' '},
                };
            size_t written = 0;
            int matched = 0;
            for (size_t i = 0; i < sizeof named / sizeof named[0]; i++) {
                size_t length = strlen(named[i].name);
                if ((size_t) (end - at) >= length
                    && memcmp(at, named[i].name, length) == 0) {
                    written = put_utf8(out + used, size - 1 - used,
                                       named[i].codepoint);
                    at += length;
                    matched = 1;
                    break;
                }
            }
            if (!matched && end - at > 3 && at[1] == '#') {
                unsigned value = 0;
                const char *digit = at + 2;
                int hex = *digit == 'x' || *digit == 'X';
                if (hex) digit++;
                const char *start = digit;
                while (digit < end && *digit != ';') {
                    char c = *digit;
                    unsigned d;
                    if (c >= '0' && c <= '9') d = (unsigned) (c - '0');
                    else if (hex && c >= 'a' && c <= 'f') d = 10u + (unsigned) (c - 'a');
                    else if (hex && c >= 'A' && c <= 'F') d = 10u + (unsigned) (c - 'A');
                    else break;
                    value = value * (hex ? 16u : 10u) + d;
                    if (value > 0x10FFFFu) break;
                    digit++;
                }
                if (digit < end && *digit == ';' && digit > start) {
                    written = put_utf8(out + used, size - 1 - used, value);
                    at = digit + 1;
                    matched = 1;
                }
            }
            if (matched) {
                if (written == 0) break;
                used += written;
                continue;
            }
        }
        /* Keep whole UTF-8 sequences together. */
        unsigned char lead = (unsigned char) *at;
        size_t length = lead < 0x80u ? 1 : lead >= 0xF0u ? 4
            : lead >= 0xE0u ? 3 : lead >= 0xC0u ? 2 : 1;
        if ((size_t) (end - at) < length || used + length >= size) break;
        memcpy(out + used, at, length);
        used += length;
        at += length;
    }
    out[used] = '\0';
}

/* Text of the first <span class=NAME> ... </span> inside [at, end), where
   the span holds no nested span. */
static int span_text(const char *at, const char *end, const char *name,
                     char *out, size_t size)
{
    char open[48];
    snprintf(open, sizeof open, "<span class=%s>", name);
    const char *start = find(at, end, open);
    if (start == NULL) {
        if (size > 0) out[0] = '\0';
        return 0;
    }
    start += strlen(open);
    const char *stop = find(start, end, "</span>");
    if (stop == NULL) stop = end;
    copy_text(start, stop, out, size);
    return 1;
}

static int valid_id(const char *id, size_t length)
{
    if (length != 11) return 0;
    for (size_t i = 0; i < length; i++) {
        char c = id[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
              || (c >= '0' && c <= '9') || c == '-' || c == '_'))
            return 0;
    }
    return 1;
}

size_t yt_parse_cards(const char *html, size_t length, YtVideo *videos,
                      size_t limit)
{
    static const char row[] = "<div class=result-row>";
    static const char watch[] = "https://www.youtube.com/watch?v=";
    const char *end = html + length;
    const char *at = find(html, end, row);
    size_t count = 0;
    while (at != NULL && count < limit) {
        const char *next = find(at + sizeof row - 1, end, row);
        const char *card_end = next == NULL ? end : next;
        const char *href = find(at, card_end, watch);
        if (href != NULL) {
            const char *id = href + sizeof watch - 1;
            size_t id_length = 0;
            while (id + id_length < card_end && id[id_length] != '"'
                   && id[id_length] != '&')
                id_length++;
            if (valid_id(id, id_length)) {
                YtVideo *video = &videos[count];
                memset(video, 0, sizeof *video);
                memcpy(video->id, id, id_length);
                span_text(at, card_end, "duration", video->duration,
                          sizeof video->duration);
                span_text(at, card_end, "title", video->title,
                          sizeof video->title);
                span_text(at, card_end, "channel", video->channel,
                          sizeof video->channel);
                /* The second meta span holds views and date; the first
                   wraps the channel. */
                const char *channel = find(at, card_end, "<span class=channel>");
                const char *after = channel == NULL ? at
                    : find(channel, card_end, "</span></span>");
                if (after != NULL)
                    span_text(after, card_end, "meta", video->meta,
                              sizeof video->meta);
                count++;
            }
        }
        at = next;
    }
    return count;
}

int yt_more_url(const char *html, size_t length, char *url, size_t size)
{
    static const char marker[] = "<a class=more href=\"";
    const char *end = html + length;
    const char *at = find(html, end, marker);
    if (at == NULL || size == 0) return 0;
    at += sizeof marker - 1;
    const char *close = find(at, end, "\"");
    if (close == NULL || find(at, close, "tilefinch_token=") == NULL) return 0;
    copy_text(at, close, url, size);
    return url[0] != '\0' && strlen(url) + 1 < size;
}

int yt_search_url(const char *query, char *url, size_t size)
{
    static const char prefix[] = "https://m.youtube.com/results?search_query=";
    static const char hex[] = "0123456789ABCDEF";
    size_t used = sizeof prefix - 1;
    if (query == NULL || size <= used) return 0;
    memcpy(url, prefix, used);
    int any = 0;
    for (const unsigned char *at = (const unsigned char *) query; *at; at++) {
        unsigned char c = *at;
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
            || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.'
            || c == '~') {
            if (used + 1 >= size) return 0;
            url[used++] = (char) c;
            any = 1;
        } else if (c == ' ') {
            if (used + 1 >= size) return 0;
            url[used++] = '+';
        } else {
            if (used + 3 >= size) return 0;
            url[used++] = '%';
            url[used++] = hex[c >> 4];
            url[used++] = hex[c & 15u];
            any = 1;
        }
    }
    url[used] = '\0';
    return any;
}
