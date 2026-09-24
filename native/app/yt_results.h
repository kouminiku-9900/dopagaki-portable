/* Video lists out of tilefinch's YouTube documents.
 *
 * youtube_lite turns YouTube's search and watch responses into a small,
 * machine-written HTML document. Every video in it is one card:
 *
 *   <div class=result-row><a class=card ... href="https://www.youtube.com/
 *   watch?v=ID"> ... <span class=duration>4:05</span> ...
 *   <span class=title>...</span><span class=meta><span class=channel>...
 *   </span></span><span class=meta>views &middot; published</span> ...
 *
 * (lite_html_video in vendor/tilefinch/src/youtube_lite.c). This reads those
 * cards back into plain records, so the native screens get a list without a
 * browser. Portable C with no PSP dependency; tested on the Mac.
 */
#ifndef KOMI_YT_RESULTS_H
#define KOMI_YT_RESULTS_H

#include <stddef.h>

#define YT_ID_SIZE 12
#define YT_TITLE_SIZE 256
#define YT_CHANNEL_SIZE 128
#define YT_SHORT_SIZE 80

typedef struct {
    char id[YT_ID_SIZE];
    char title[YT_TITLE_SIZE];
    char channel[YT_CHANNEL_SIZE];
    char duration[YT_SHORT_SIZE];
    /* "views · published", as one line */
    char meta[YT_SHORT_SIZE];
} YtVideo;

/* Parse up to `limit` cards in document order; returns how many. */
size_t yt_parse_cards(const char *html, size_t length, YtVideo *videos,
                      size_t limit);

/* The "Load more results" link youtube_lite puts after a search page
   (<a class=more href="...tilefinch_token=...">), unescaped. Returns 0 when
   the page has none. */
int yt_more_url(const char *html, size_t length, char *url, size_t size);

/* https://m.youtube.com/results?search_query=<UTF-8, percent-encoded>.
   Returns 0 when the query is empty or does not fit. */
int yt_search_url(const char *query, char *url, size_t size);

#endif
