#include "shorts_parse.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* --- scanning --- */

static const char *find(const char *from, const char *end, const char *key)
{
    size_t key_length = strlen(key);
    if (from == NULL || key_length == 0) return NULL;
    for (const char *at = from; at + key_length <= end; at++) {
        at = memchr(at, key[0], (size_t) (end - at));
        if (at == NULL || at + key_length > end) return NULL;
        if (memcmp(at, key, key_length) == 0) return at;
    }
    return NULL;
}

static const char *find_last(const char *from, const char *end,
                             const char *key)
{
    const char *last = NULL;
    for (const char *at = find(from, end, key); at != NULL;
         at = find(at + 1, end, key))
        last = at;
    return last;
}

static size_t put_utf8(char *out, size_t size, size_t used, uint32_t cp)
{
    char bytes[4];
    size_t n;
    if (cp < 0x80u) {
        bytes[0] = (char) cp;
        n = 1;
    } else if (cp < 0x800u) {
        bytes[0] = (char) (0xC0u | (cp >> 6));
        bytes[1] = (char) (0x80u | (cp & 0x3Fu));
        n = 2;
    } else if (cp < 0x10000u) {
        bytes[0] = (char) (0xE0u | (cp >> 12));
        bytes[1] = (char) (0x80u | ((cp >> 6) & 0x3Fu));
        bytes[2] = (char) (0x80u | (cp & 0x3Fu));
        n = 3;
    } else {
        bytes[0] = (char) (0xF0u | (cp >> 18));
        bytes[1] = (char) (0x80u | ((cp >> 12) & 0x3Fu));
        bytes[2] = (char) (0x80u | ((cp >> 6) & 0x3Fu));
        bytes[3] = (char) (0x80u | (cp & 0x3Fu));
        n = 4;
    }
    /* Never split a character: stop instead. */
    if (used + n >= size) return used;
    memcpy(out + used, bytes, n);
    return used + n;
}

static int hex4(const char *at, const char *end, uint32_t *value)
{
    if (at + 4 > end) return 0;
    uint32_t v = 0;
    for (int i = 0; i < 4; i++) {
        char c = at[i];
        int d = c >= '0' && c <= '9' ? c - '0'
            : c >= 'a' && c <= 'f' ? c - 'a' + 10
            : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
        if (d < 0) return 0;
        v = (v << 4) | (uint32_t) d;
    }
    *value = v;
    return 1;
}

/* Decode the JSON string whose body starts at `at` (just past the opening
   quote) into out, truncated to fit. Returns false when unterminated. */
static bool read_string(const char *at, const char *end, char *out,
                        size_t size)
{
    size_t used = 0;
    if (size == 0) return false;
    while (at < end && *at != '"') {
        if (*at != '\\') {
            if (used + 1 < size) out[used++] = *at;
            at++;
            continue;
        }
        if (at + 1 >= end) break;
        char e = at[1];
        at += 2;
        switch (e) {
        case 'n': case 'r': case 't': case 'b': case 'f':
            if (used + 1 < size) out[used++] = ' ';
            break;
        case 'u': {
            uint32_t cp = 0, low = 0;
            if (!hex4(at, end, &cp)) goto done;
            at += 4;
            if (cp >= 0xD800u && cp < 0xDC00u && at + 6 <= end
                && at[0] == '\\' && at[1] == 'u' && hex4(at + 2, end, &low)
                && low >= 0xDC00u && low < 0xE000u) {
                cp = 0x10000u + ((cp - 0xD800u) << 10) + (low - 0xDC00u);
                at += 6;
            }
            used = put_utf8(out, size, used, cp);
            break;
        }
        default:
            if (used + 1 < size) out[used++] = e;
            break;
        }
    }
done:
    out[used] = '\0';
    return at < end && *at == '"';
}

/* The string value right after key (key ends with the opening quote). */
static bool value_after(const char *from, const char *end, const char *key,
                        char *out, size_t size)
{
    const char *at = find(from, end, key);
    if (at == NULL) return false;
    return read_string(at + strlen(key), end, out, size);
}

static bool valid_id(const char *id)
{
    if (strlen(id) != 11) return false;
    for (const char *c = id; *c != '\0'; c++)
        if (!((*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z')
              || (*c >= '0' && *c <= '9') || *c == '-' || *c == '_'))
            return false;
    return true;
}

static ShortItem *add_item(ShortBatch *batch, const char *id)
{
    if (!valid_id(id) || batch->count >= batch->capacity) return NULL;
    for (size_t i = 0; i < batch->count; i++)
        if (strcmp(batch->items[i].id, id) == 0) return NULL;
    ShortItem *item = &batch->items[batch->count++];
    memset(item, 0, sizeof *item);
    snprintf(item->id, sizeof item->id, "%s", id);
    return item;
}

static void reset(ShortBatch *batch)
{
    batch->count = 0;
    batch->token[0] = '\0';
}

#define REEL_ID "\"reelWatchEndpoint\":{\"videoId\":\""

/* --- the three answers --- */

bool shorts_parse_seedless(const char *json, size_t length, ShortBatch *batch)
{
    reset(batch);
    const char *end = json + length;
    const char *replacement = find(json, end, "\"replacementEndpoint\":{");
    char id[SHORT_ID_SIZE + 8];
    if (replacement != NULL
        && value_after(replacement, end, "\"videoId\":\"", id, sizeof id))
        (void) add_item(batch, id);
    (void) value_after(json, end, "\"sequenceContinuation\":\"",
                       batch->token, sizeof batch->token);
    return batch->count > 0 || batch->token[0] != '\0';
}

bool shorts_parse_sequence(const char *json, size_t length, ShortBatch *batch)
{
    reset(batch);
    const char *end = json + length;
    const char *entries = find(json, end, "\"entries\":[");
    const char *continuation = find(json, end, "\"continuationEndpoint\":");
    const char *stop = continuation != NULL ? continuation : end;
    for (const char *at = entries == NULL ? NULL : find(entries, stop, REEL_ID);
         at != NULL; at = find(at + 1, stop, REEL_ID)) {
        char id[SHORT_ID_SIZE + 8];
        if (read_string(at + strlen(REEL_ID), stop, id, sizeof id))
            (void) add_item(batch, id);
    }
    if (continuation != NULL)
        (void) value_after(continuation, end, "\"token\":\"", batch->token,
                           sizeof batch->token);
    return batch->count > 0;
}

/* "TITLE, 2,905回視聴, CHANNEL, 1 時間前 - ショート動画を再生": the channel
   is the second field from the right once the trailing " - ..." is gone.
   Split from the right because titles may hold ", " themselves. */
static void channel_from_accessibility(const char *text, char *out,
                                       size_t size)
{
    out[0] = '\0';
    char copy[768];
    snprintf(copy, sizeof copy, "%s", text);
    char *dash = NULL;
    for (char *at = strstr(copy, " - "); at != NULL;
         at = strstr(at + 1, " - "))
        dash = at;
    if (dash != NULL) *dash = '\0';
    char *last = NULL, *before = NULL;
    for (char *at = strstr(copy, ", "); at != NULL; at = strstr(at + 1, ", ")) {
        before = last;
        last = at;
    }
    if (last == NULL || before == NULL) return;
    *last = '\0';
    snprintf(out, size, "%s", before + 2);
}

bool shorts_parse_search(const char *json, size_t length, ShortBatch *batch)
{
    static const char card_key[] = "\"shortsLockupViewModel\":{";
    reset(batch);
    const char *end = json + length;
    const char *card = find(json, end, card_key);
    while (card != NULL) {
        const char *next = find(card + 1, end, card_key);
        const char *stop = next != NULL ? next : end;
        char id[SHORT_ID_SIZE + 8];
        if (value_after(card, stop, REEL_ID, id, sizeof id)) {
            ShortItem *item = add_item(batch, id);
            if (item != NULL) {
                static char accessibility[768];
                if (!value_after(card, stop,
                                 "\"overlayMetadata\":{\"primaryText\":"
                                 "{\"content\":\"",
                                 item->title, sizeof item->title))
                    item->title[0] = '\0';
                if (value_after(card, stop, "\"accessibilityText\":\"",
                                accessibility, sizeof accessibility)) {
                    channel_from_accessibility(accessibility, item->channel,
                                               sizeof item->channel);
                    if (item->title[0] == '\0') {
                        char *comma = strstr(accessibility, ", ");
                        if (comma != NULL) *comma = '\0';
                        snprintf(item->title, sizeof item->title, "%s",
                                 accessibility);
                    }
                }
            }
        }
        card = next;
    }
    const char *command = find_last(json, end,
                                    "\"continuationCommand\":{\"token\":\"");
    if (command != NULL)
        (void) read_string(command + strlen(
                               "\"continuationCommand\":{\"token\":\""),
                           end, batch->token, sizeof batch->token);
    return batch->count > 0;
}

bool shorts_json_quote(const char *text, char *out, size_t size)
{
    size_t used = 0;
    if (size < 3) return false;
    out[used++] = '"';
    for (const unsigned char *c = (const unsigned char *) text; *c != '\0';
         c++) {
        char escaped[8];
        size_t n;
        if (*c == '"' || *c == '\\') {
            escaped[0] = '\\';
            escaped[1] = (char) *c;
            n = 2;
        } else if (*c < 0x20u) {
            n = (size_t) snprintf(escaped, sizeof escaped, "\\u%04x", *c);
        } else {
            escaped[0] = (char) *c;
            n = 1;
        }
        if (used + n + 2 > size) return false;
        memcpy(out + used, escaped, n);
        used += n;
    }
    out[used++] = '"';
    out[used] = '\0';
    return true;
}
