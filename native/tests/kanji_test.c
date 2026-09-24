/* kanji.c against saved service responses (and awkward input). */
#include <stdio.h>
#include <string.h>

#include "kanji.h"

static int failures;

#define CHECK(condition) \
    do { \
        if (!(condition)) { \
            printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #condition); \
            failures++; \
        } \
    } while (0)

int main(void)
{
    static const char one[] =
        "[[\"ねこかわいい\",[\"猫かわいい\",\"ねこかわいい\",\"猫可愛い\","
        "\"ネコ可愛い\",\"ネコカワイイ\"]]]";
    KanjiConversion c;
    CHECK(kanji_parse_transliteration(one, sizeof one - 1, &c));
    CHECK(c.count == 1);
    CHECK(c.segments[0].count == 5);
    CHECK(strcmp(c.segments[0].reading, "ねこかわいい") == 0);
    CHECK(strcmp(c.segments[0].candidates[0], "猫かわいい") == 0);

    /* Two segments, with \u escapes and spaces. */
    static const char two[] =
        "[ [\"きょうの\", [\"今日の\",\"\\u4eca\\u65e5\\u306e\"]] ,"
        " [\"てんき\",[\"天気\",\"転機\",\"天気\",\"てんき\",\"テンキ\","
        "\"extra\"]] ]";
    CHECK(kanji_parse_transliteration(two, sizeof two - 1, &c));
    CHECK(c.count == 2);
    CHECK(strcmp(c.segments[0].candidates[1], "今日の") == 0);
    CHECK(c.segments[1].count == KANJI_CANDIDATES);
    CHECK(strcmp(c.segments[1].candidates[1], "転機") == 0);

    CHECK(!kanji_parse_transliteration("<html>", 6, &c));
    CHECK(!kanji_parse_transliteration("[", 1, &c));

    static const char suggest[] =
        "[\"猫\",[\"猫\",\"猫ミーム\",\"猫 かわいい\"],[],"
        "{\"google:suggestsubtypes\":[[512],[512,433],[512]]}]";
    char out[KANJI_SUGGESTIONS][KANJI_TEXT];
    CHECK(kanji_parse_suggestions(suggest, sizeof suggest - 1, out,
                                  KANJI_SUGGESTIONS) == 3);
    CHECK(strcmp(out[1], "猫ミーム") == 0);
    CHECK(kanji_parse_suggestions(suggest, sizeof suggest - 1, out, 2) == 2);

    CHECK(kanji_has_hiragana("ねこ"));
    CHECK(kanji_has_hiragana("猫の"));
    CHECK(!kanji_has_hiragana("猫"));
    CHECK(!kanji_has_hiragana("ネコ cat"));

    char url[64];
    CHECK(kanji_url_encode("猫 a", url, sizeof url));
    CHECK(strcmp(url, "%E7%8C%AB%20a") == 0);
    CHECK(!kanji_url_encode("猫猫猫猫猫猫", url, 8));

    printf("kanji_test: %s\n", failures == 0 ? "PASS" : "FAIL");
    return failures == 0 ? 0 : 1;
}
