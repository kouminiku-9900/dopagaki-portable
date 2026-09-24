/* mylist round trip: add, move-to-front, remove, reload, awkward text. */
#include <stdio.h>
#include <string.h>

#include "mylist.h"

static int failures;

#define CHECK(condition) \
    do { \
        if (!(condition)) { \
            printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #condition); \
            failures++; \
        } \
    } while (0)

static YtVideo video(const char *id, const char *title)
{
    YtVideo v;
    memset(&v, 0, sizeof v);
    snprintf(v.id, sizeof v.id, "%s", id);
    snprintf(v.title, sizeof v.title, "%s", title);
    snprintf(v.channel, sizeof v.channel, "チャンネル");
    snprintf(v.duration, sizeof v.duration, "4:05");
    return v;
}

int main(int argc, char **argv)
{
    const char *path = argc > 1 ? argv[1] : "mylist-test.txt";
    remove(path);
    static Mylist list;
    CHECK(mylist_load(&list, path));
    CHECK(list.count == 0);

    YtVideo a = video("aaaaaaaaaaa", "猫の動画");
    YtVideo b = video("bbbbbbbbbbb", "tab\tin\ntitle");
    YtVideo c = video("ccccccccccc", "三本目");
    CHECK(mylist_add(&list, &a));
    CHECK(mylist_add(&list, &b));
    CHECK(mylist_add(&list, &c));
    CHECK(list.count == 3);
    CHECK(strcmp(list.items[0].id, "ccccccccccc") == 0);
    /* Liking again moves it to the front without a duplicate. */
    CHECK(mylist_add(&list, &a));
    CHECK(list.count == 3);
    CHECK(strcmp(list.items[0].id, "aaaaaaaaaaa") == 0);
    CHECK(strcmp(list.items[1].id, "ccccccccccc") == 0);
    CHECK(mylist_remove(&list, "ccccccccccc"));
    CHECK(!mylist_remove(&list, "ccccccccccc"));
    CHECK(list.count == 2);

    static Mylist again;
    CHECK(mylist_load(&again, path));
    CHECK(again.count == 2);
    CHECK(strcmp(again.items[0].title, "猫の動画") == 0);
    CHECK(strcmp(again.items[0].channel, "チャンネル") == 0);
    CHECK(strcmp(again.items[1].title, "tab in title") == 0);
    CHECK(mylist_find(&again, "bbbbbbbbbbb") == 1);

    /* Full list drops the oldest. */
    for (int i = 0; i < MYLIST_CAPACITY + 5; i++) {
        char id[12];
        snprintf(id, sizeof id, "x%010d", i);
        YtVideo v = video(id, "many");
        CHECK(mylist_add(&again, &v));
    }
    CHECK(again.count == MYLIST_CAPACITY);
    CHECK(strcmp(again.items[0].id, "x0000000204") == 0);
    remove(path);
    printf("mylist_test: %s\n", failures == 0 ? "PASS" : "FAIL");
    return failures == 0 ? 0 : 1;
}
