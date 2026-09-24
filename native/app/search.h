/* Video lists for the native screens -- a search page, the next page of a
 * search, or a watch page's related videos -- through tilefinch's
 * youtube_lite (the same JSON APIs the browser version uses), pumped a slice
 * at a time so the caller can keep drawing. */
#ifndef KOMI_SEARCH_H
#define KOMI_SEARCH_H

#include <stdbool.h>
#include <stddef.h>

#include "yt_results.h"

#define SEARCH_RESULTS 20
#define SEARCH_URL_SIZE 2048

typedef struct SearchJob SearchJob;

typedef enum {
    SEARCH_PENDING,
    SEARCH_DONE,
    SEARCH_FAILED
} SearchStatus;

/* url: a youtube_lite route (results?search_query=..., the next-page link,
   or m.youtube.com/watch?v=ID for related videos). NULL (with error) when
   the request could not start. */
SearchJob *search_begin_url(const char *url, char *error, size_t error_size);
/* Advance a bounded slice. On DONE the videos are in `videos`, and
   `more_url` holds the next page's URL or "". */
SearchStatus search_pump(SearchJob *job, YtVideo *videos, size_t *count,
                         char *more_url, size_t more_size,
                         char *error, size_t error_size);
void search_end(SearchJob *job);

#endif
