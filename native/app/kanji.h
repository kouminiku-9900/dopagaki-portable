/* Kana-to-kanji for search, over the network.
 *
 * The firmware keyboard does not convert kana to kanji when a homebrew
 * program opens it, so the app converts the typed text itself:
 *   - Google's transliteration service splits the hiragana into segments and
 *     returns up to five candidates per segment
 *     (https://www.google.com/transliterate?langpair=ja-Hira|ja&text=...);
 *   - YouTube's search suggestions for the converted text add ready-made
 *     queries (suggestqueries.google.com, client=firefox, ds=yt).
 * The parsing is portable and tested on the Mac with saved responses. */
#ifndef KOMI_KANJI_H
#define KOMI_KANJI_H

#include <stdbool.h>
#include <stddef.h>

#define KANJI_SEGMENTS 8
#define KANJI_CANDIDATES 5
#define KANJI_TEXT 96
#define KANJI_SUGGESTIONS 10

typedef struct {
    char reading[KANJI_TEXT];
    char candidates[KANJI_CANDIDATES][KANJI_TEXT];
    size_t count;
} KanjiSegment;

typedef struct {
    KanjiSegment segments[KANJI_SEGMENTS];
    size_t count;
} KanjiConversion;

/* [["reading",["cand",...]],...] */
bool kanji_parse_transliteration(const char *json, size_t length,
                                 KanjiConversion *conversion);
/* ["query",["suggestion",...],...] -> up to limit suggestions. */
size_t kanji_parse_suggestions(const char *json, size_t length,
                               char out[][KANJI_TEXT], size_t limit);
/* True when text holds any hiragana (U+3041..U+3096). */
bool kanji_has_hiragana(const char *text);
/* Percent-encode UTF-8 for a query string. */
bool kanji_url_encode(const char *text, char *out, size_t size);

#endif
