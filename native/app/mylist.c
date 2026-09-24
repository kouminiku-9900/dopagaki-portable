#include "mylist.h"

#include <stdio.h>
#include <string.h>

/* Copy one tab-separated field, turning stray tabs and newlines in titles
   into spaces so a line always stays one record. */
static void put_field(FILE *file, const char *text)
{
    for (const char *at = text; *at != '\0'; at++) {
        char c = *at;
        fputc(c == '\t' || c == '\n' || c == '\r' ? ' ' : c, file);
    }
}

static void take_field(char **cursor, char *out, size_t size)
{
    char *at = *cursor;
    size_t length = strcspn(at, "\t\r\n");
    size_t copy = length < size - 1 ? length : size - 1;
    /* Do not cut a UTF-8 sequence in half. */
    while (copy > 0 && copy < length
           && ((unsigned char) at[copy] & 0xC0u) == 0x80u)
        copy--;
    memcpy(out, at, copy);
    out[copy] = '\0';
    at += length;
    if (*at == '\t') at++;
    *cursor = at;
}

bool mylist_load(Mylist *list, const char *path)
{
    memset(list, 0, sizeof *list);
    snprintf(list->path, sizeof list->path, "%s", path);
    FILE *file = fopen(path, "r");
    if (file == NULL) return true;
    char line[1024];
    while (list->count < MYLIST_CAPACITY
           && fgets(line, sizeof line, file) != NULL) {
        char *cursor = line;
        YtVideo *video = &list->items[list->count];
        memset(video, 0, sizeof *video);
        take_field(&cursor, video->id, sizeof video->id);
        if (strlen(video->id) != 11) continue;
        take_field(&cursor, video->title, sizeof video->title);
        take_field(&cursor, video->channel, sizeof video->channel);
        take_field(&cursor, video->duration, sizeof video->duration);
        if (mylist_find(list, video->id) >= 0) continue;
        list->count++;
    }
    fclose(file);
    return true;
}

bool mylist_save(const Mylist *list)
{
    char temporary[sizeof list->path + 8];
    snprintf(temporary, sizeof temporary, "%s.tmp", list->path);
    FILE *file = fopen(temporary, "w");
    if (file == NULL) return false;
    for (size_t i = 0; i < list->count; i++) {
        const YtVideo *video = &list->items[i];
        put_field(file, video->id);
        fputc('\t', file);
        put_field(file, video->title);
        fputc('\t', file);
        put_field(file, video->channel);
        fputc('\t', file);
        put_field(file, video->duration);
        fputc('\n', file);
    }
    bool written = fflush(file) == 0;
    written = fclose(file) == 0 && written;
    if (!written) {
        remove(temporary);
        return false;
    }
    /* rename() over an existing file is not atomic on every PSP device
       driver; remove the old one first and accept the tiny window. */
    remove(list->path);
    return rename(temporary, list->path) == 0;
}

int mylist_find(const Mylist *list, const char *id)
{
    for (size_t i = 0; i < list->count; i++) {
        if (strcmp(list->items[i].id, id) == 0) return (int) i;
    }
    return -1;
}

bool mylist_add(Mylist *list, const YtVideo *video)
{
    YtVideo copy = *video;
    int at = mylist_find(list, video->id);
    size_t shift = at >= 0 ? (size_t) at
        : list->count < MYLIST_CAPACITY ? list->count : MYLIST_CAPACITY - 1;
    memmove(&list->items[1], &list->items[0], shift * sizeof copy);
    list->items[0] = copy;
    if (at < 0 && list->count < MYLIST_CAPACITY) list->count++;
    return mylist_save(list);
}

bool mylist_remove(Mylist *list, const char *id)
{
    int at = mylist_find(list, id);
    if (at < 0) return false;
    memmove(&list->items[at], &list->items[at + 1],
            (list->count - (size_t) at - 1) * sizeof list->items[0]);
    list->count--;
    return mylist_save(list);
}
