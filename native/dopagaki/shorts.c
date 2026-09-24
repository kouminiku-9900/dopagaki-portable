#include "shorts.h"

#include <stdio.h>
#include <string.h>

#include "tilefinch/fetch.h"

#include "komi_runtime.h"

/* The MWEB client version m.youtube.com announced on 2026-09-22. InnerTube
   accepts older versions for a long time; update it when requests start
   failing. */
#define MWEB_VERSION "2.20260922.04.00"
#define MOBILE_UA \
    "Mozilla/5.0 (Linux; Android 10; K) AppleWebKit/537.36 " \
    "(KHTML, like Gecko) Chrome/136.0.0.0 Mobile Safari/537.36"
#define ANSWER_LIMIT (512u * 1024u)
#define TIMEOUT_MS 15000
/* The Shorts filter of the search page ("Type: Shorts"). */
#define SHORTS_FILTER "EgIQCQ%3D%3D"

typedef bool (*Parser)(const char *json, size_t length, ShortBatch *batch);

/* POST {"context":...,<fields>} to youtubei/v1/<endpoint> and parse. */
static bool post(const char *endpoint, const char *fields, Parser parse,
                 ShortBatch *batch)
{
    static char body[SHORT_TOKEN_SIZE + 1024];
    char url[160];
    snprintf(url, sizeof url,
             "https://m.youtube.com/youtubei/v1/%s?prettyPrint=false",
             endpoint);
    int body_length = snprintf(
        body, sizeof body,
        "{\"context\":{\"client\":{\"clientName\":\"MWEB\","
        "\"clientVersion\":\"" MWEB_VERSION "\",\"hl\":\"ja\","
        "\"gl\":\"JP\"}},%s}", fields);
    if (body_length < 0 || (size_t) body_length >= sizeof body) return false;
    FetchRequest request = {
        .method = "POST",
        .body = body,
        .body_length = (size_t) body_length,
        .content_type = "application/json",
        .extra_headers = "X-YouTube-Client-Name: 2\n"
                         "X-YouTube-Client-Version: " MWEB_VERSION,
        .origin = "https://m.youtube.com",
        .accept = "application/json",
        .sec_fetch_dest = "empty",
        .sec_fetch_mode = "cors",
        .sec_fetch_site = "same-origin",
        .user_agent = MOBILE_UA,
        .credentials = FETCH_CREDENTIALS_OMIT,
        .initiator_url = "https://m.youtube.com/",
        .redirect_same_origin_only = true,
    };
    uint64_t started = komi_now_us();
    FetchResult result = {0};
    bool fetched = fetch_request_cancelable(&komi.budget, url, &request,
                                            ANSWER_LIMIT, TIMEOUT_MS, NULL,
                                            NULL, &result)
        && result.status_code == 200 && result.data != NULL;
    bool parsed = fetched && parse(result.data, result.length, batch);
    komi_result("shorts %s %s status=%ld bytes=%u items=%u token=%d "
                "elapsed=%llums",
                endpoint, parsed ? "OK" : "FAIL", result.status_code,
                (unsigned) result.length, (unsigned) batch->count,
                batch->token[0] != '\0',
                (unsigned long long) ((komi_now_us() - started) / 1000u));
    fetch_result_free(&result);
    return parsed;
}

bool shorts_fetch_seedless(ShortBatch *batch)
{
    return post("reel/reel_item_watch",
                "\"params\":\"CA8%3D\","
                "\"inputType\":\"REEL_WATCH_INPUT_TYPE_SEEDLESS\","
                "\"disablePlayerResponse\":true",
                shorts_parse_seedless, batch);
}

static bool post_token(const char *endpoint, const char *key,
                       const char *token, Parser parse, ShortBatch *batch)
{
    static char fields[SHORT_TOKEN_SIZE + 64];
    char quoted[SHORT_TOKEN_SIZE + 8];
    if (!shorts_json_quote(token, quoted, sizeof quoted)) return false;
    snprintf(fields, sizeof fields, "\"%s\":%s", key, quoted);
    return post(endpoint, fields, parse, batch);
}

bool shorts_fetch_sequence(const char *token, ShortBatch *batch)
{
    return post_token("reel/reel_watch_sequence", "sequenceParams", token,
                      shorts_parse_sequence, batch);
}

bool shorts_fetch_search(const char *query, ShortBatch *batch)
{
    char quoted[512];
    char fields[600];
    if (!shorts_json_quote(query, quoted, sizeof quoted)) return false;
    snprintf(fields, sizeof fields,
             "\"query\":%s,\"params\":\"" SHORTS_FILTER "\"", quoted);
    return post("search", fields, shorts_parse_search, batch);
}

bool shorts_fetch_search_more(const char *token, ShortBatch *batch)
{
    return post_token("search", "continuation", token, shorts_parse_search,
                      batch);
}
