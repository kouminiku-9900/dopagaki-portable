/* YouTube Shorts lists out of the InnerTube JSON the MWEB client gets.
 *
 * Three answers carry Shorts, and each is read by scanning for the few keys
 * that matter (the answers are 20-230 KB; building a tree is not needed):
 *
 *   reel/reel_item_watch   (seedless: the Shorts tab without a video)
 *       "replacementEndpoint":{"reelWatchEndpoint":{"videoId":"ID",...}}
 *       "sequenceContinuation":"TOKEN"
 *   reel/reel_watch_sequence  (the next Shorts after TOKEN)
 *       "entries":[{"command":{..."reelWatchEndpoint":{"videoId":"ID"...
 *       "continuationEndpoint":{..."continuationCommand":{"token":"TOKEN"
 *   search with the Shorts filter  (params EgIQCQ==, and its continuation)
 *       "shortsLockupViewModel":{..."accessibilityText":"TITLE, VIEWS,
 *       CHANNEL, WHEN - ショート動画を再生",..."reelWatchEndpoint":
 *       {"videoId":"ID"...,"overlayMetadata":{"primaryText":{"content":
 *       "TITLE"},...
 *       "continuationCommand":{"token":"TOKEN"  (the last one)
 *
 * Portable C with no PSP dependency; tested on the Mac with saved answers
 * (native/tests/fixtures/shorts-*.json). */
#ifndef DOPAGAKI_SHORTS_PARSE_H
#define DOPAGAKI_SHORTS_PARSE_H

#include <stdbool.h>
#include <stddef.h>

#define SHORT_ID_SIZE 12
#define SHORT_TITLE_SIZE 256
#define SHORT_CHANNEL_SIZE 128
#define SHORT_TOKEN_SIZE 2048

typedef struct {
    char id[SHORT_ID_SIZE];
    /* May be empty (sequence entries carry only the ID; the title comes
       from the player once the Short is opened). */
    char title[SHORT_TITLE_SIZE];
    char channel[SHORT_CHANNEL_SIZE];
} ShortItem;

/* Items found in one answer, in order, without duplicates of each other.
   `token` is the continuation for the next batch, or "". */
typedef struct {
    ShortItem *items;
    size_t capacity;
    size_t count;
    char token[SHORT_TOKEN_SIZE];
} ShortBatch;

/* reel_item_watch (seedless): one item and the sequence token. */
bool shorts_parse_seedless(const char *json, size_t length, ShortBatch *batch);
/* reel_watch_sequence: the entries and the next sequence token. */
bool shorts_parse_sequence(const char *json, size_t length, ShortBatch *batch);
/* search (Shorts filter) or its continuation: cards with titles. */
bool shorts_parse_search(const char *json, size_t length, ShortBatch *batch);

/* JSON string body for a query, quoted: "猫" -> "\"猫\"". */
bool shorts_json_quote(const char *text, char *out, size_t size);

#endif
