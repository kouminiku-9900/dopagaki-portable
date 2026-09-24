/* Mac-side test of native/dopagaki/shorts_parse.c against saved answers:
 *   shorts_test <fixtures dir>
 * Prints what was found and exits non-zero when an answer yields nothing. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "shorts_parse.h"

static char *slurp(const char *dir, const char *name, size_t *length)
{
    char path[1024];
    snprintf(path, sizeof path, "%s/%s", dir, name);
    FILE *file = fopen(path, "rb");
    if (file == NULL) return NULL;
    fseek(file, 0, SEEK_END);
    long size = ftell(file);
    fseek(file, 0, SEEK_SET);
    char *data = malloc((size_t) size + 1);
    *length = fread(data, 1, (size_t) size, file);
    data[*length] = '\0';
    fclose(file);
    return data;
}

static ShortItem items[64];

static int run(const char *dir, const char *name,
               bool (*parse)(const char *, size_t, ShortBatch *),
               size_t minimum, int want_titles)
{
    size_t length = 0;
    char *json = slurp(dir, name, &length);
    if (json == NULL) {
        printf("%s: missing\n", name);
        return 1;
    }
    ShortBatch batch = {items, 64, 0, ""};
    bool ok = parse(json, length, &batch);
    size_t titled = 0;
    for (size_t i = 0; i < batch.count; i++) {
        if (items[i].title[0] != '\0') titled++;
        if (i < 3)
            printf("  %s | %s | %s\n", items[i].id, items[i].title,
                   items[i].channel);
    }
    int failed = !ok || batch.count < minimum || batch.token[0] == '\0'
        || (want_titles && titled != batch.count);
    printf("%s: %s items=%zu titled=%zu token=%zu bytes\n", name,
           failed ? "FAIL" : "PASS", batch.count, titled,
           strlen(batch.token));
    free(json);
    return failed;
}

int main(int argc, char **argv)
{
    const char *dir = argc > 1 ? argv[1] : "native/tests/fixtures";
    int failures = 0;
    failures += run(dir, "shorts-seed.json", shorts_parse_seedless, 1, 0);
    failures += run(dir, "shorts-seq.json", shorts_parse_sequence, 1, 0);
    failures += run(dir, "shorts-search.json", shorts_parse_search, 10, 1);

    char quoted[64];
    failures += !shorts_json_quote("a\"b\\c", quoted, sizeof quoted)
        || strcmp(quoted, "\"a\\\"b\\\\c\"") != 0;
    printf("json-quote: %s\n", quoted);
    return failures == 0 ? 0 : 1;
}
