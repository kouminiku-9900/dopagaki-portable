/* Shorts lists from YouTube's InnerTube API (MWEB client, no account), one
 * blocking POST each (about 1-2 s on the PSP):
 *
 *   おすすめ   reel/reel_item_watch without a video (the Shorts tab), then
 *              reel/reel_watch_sequence with the token it returns, again and
 *              again as the list runs out.
 *   検索       search with the Shorts filter, then its continuation.
 *
 * The answers are read by shorts_parse.c. */
#ifndef DOPAGAKI_SHORTS_H
#define DOPAGAKI_SHORTS_H

#include <stdbool.h>

#include "shorts_parse.h"

bool shorts_fetch_seedless(ShortBatch *batch);
bool shorts_fetch_sequence(const char *token, ShortBatch *batch);
bool shorts_fetch_search(const char *query, ShortBatch *batch);
bool shorts_fetch_search_more(const char *token, ShortBatch *batch);

#endif
