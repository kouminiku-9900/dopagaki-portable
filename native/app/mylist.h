/* マイリスト: videos the user liked, kept only on this PSP.
 *
 * One UTF-8 text file, one video per line, newest first:
 *   id<TAB>title<TAB>channel<TAB>duration
 * Saved through a temporary file and a rename, so a power cut mid-write
 * leaves the previous list. Portable C; tested on the Mac. */
#ifndef KOMI_MYLIST_H
#define KOMI_MYLIST_H

#include <stdbool.h>
#include <stddef.h>

#include "yt_results.h"

#define MYLIST_CAPACITY 200

typedef struct {
    char path[256];
    YtVideo items[MYLIST_CAPACITY];
    size_t count;
} Mylist;

/* Read the file (a missing file is an empty list). */
bool mylist_load(Mylist *list, const char *path);
bool mylist_save(const Mylist *list);
/* Index of id, or -1. */
int mylist_find(const Mylist *list, const char *id);
/* Put the video first (moving it if already there); drops the oldest when
   full. Saves. */
bool mylist_add(Mylist *list, const YtVideo *video);
/* Saves. False when id was not in the list. */
bool mylist_remove(Mylist *list, const char *id);

#endif
