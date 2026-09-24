#!/bin/sh
# Mac-side tests for native/ (no PSP needed).
#   scripts/test_native.sh            fixture tests only (offline)
#   scripts/test_native.sh --live     also search YouTube for real
# Links the host build of tilefinch (vendor/tilefinch/build-preset-release).
set -eu
PROJECT_ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
TF="$PROJECT_ROOT/vendor/tilefinch"
OUT="$PROJECT_ROOT/build/native-tests"
mkdir -p "$OUT"
# dopagaki: the Shorts answers (no tilefinch needed).
cc -std=c11 -O1 -Wall -Wextra -o "$OUT/shorts_test" \
    -I"$PROJECT_ROOT/native/dopagaki" \
    "$PROJECT_ROOT/native/tests/shorts_test.c" \
    "$PROJECT_ROOT/native/dopagaki/shorts_parse.c"
"$OUT/shorts_test" "$PROJECT_ROOT/native/tests/fixtures"
cc -std=c11 -O1 -Wall -Wextra -o "$OUT/search_probe" \
    -I"$TF/include" -I"$PROJECT_ROOT/native/app" \
    "$PROJECT_ROOT/native/tests/search_probe.c" \
    "$PROJECT_ROOT/native/app/yt_results.c" \
    -L"$TF/build-preset-release" -ltilefinch_core \
    -Wl,-rpath,"$TF/build-preset-release"
cc -std=c11 -O1 -Wall -Wextra -o "$OUT/mylist_test" \
    -I"$PROJECT_ROOT/native/app" \
    "$PROJECT_ROOT/native/tests/mylist_test.c" "$PROJECT_ROOT/native/app/mylist.c"
"$OUT/mylist_test" "$OUT/mylist-test.txt"
cc -std=c11 -O1 -Wall -Wextra -o "$OUT/kanji_test" \
    -I"$PROJECT_ROOT/native/app" \
    "$PROJECT_ROOT/native/tests/kanji_test.c" "$PROJECT_ROOT/native/app/kanji.c"
"$OUT/kanji_test"
for fixture in "$PROJECT_ROOT"/native/tests/fixtures/search-*.html; do
    [ -f "$fixture" ] || continue
    printf '%s: ' "$(basename "$fixture")"
    "$OUT/search_probe" --file "$fixture" | tail -1
done
# Saved search API responses (both card formats YouTube sends), wrapped the
# way youtube_lite_build_document reads them.
for fixture in "$PROJECT_ROOT"/native/tests/fixtures/api-search-*.html; do
    [ -f "$fixture" ] || continue
    printf '%s: ' "$(basename "$fixture")"
    "$OUT/search_probe" --json \
        'https://m.youtube.com/results?search_query=%E7%8C%AB' "$fixture" \
        | tail -1
done
if [ "${1:-}" = "--live" ]; then
    "$OUT/search_probe" "${2:-猫}" "$OUT/last-search.html"
fi
