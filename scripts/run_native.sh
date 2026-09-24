#!/bin/sh
# Run a native program on the PSP through PSPLink and collect what it wrote.
#   scripts/run_native.sh komi-player      the unattended playback test
#   scripts/run_native.sh komi-app         the client (autotest if komi-app.cfg says so)
# The PSP must be on USB with PSPLink open. The program's directory in the
# build tree is host0:, so its logs and screenshots land on the Mac; they are
# copied to field-logs/<name>-<time>/ when the module has exited.
set -eu
PROJECT_ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
NAME=${1:?usage: $0 komi-player|komi-app}
MODULE=$(printf '%s' "$NAME" | tr '-' '_')
OUT="$PROJECT_ROOT/vendor/tilefinch/build-preset-psp/$NAME"
LIMIT_SECONDS=${LIMIT_SECONDS:-1500}
"$PROJECT_ROOT/scripts/build_native.sh" "$NAME" > /dev/null
export HOST_ROOT="$OUT"
"$PROJECT_ROOT/scripts/psplink.sh" ready
running() {
    "$PROJECT_ROOT/scripts/psplink.sh" exec 'modlist' 2>/dev/null \
        | grep -q -E 'komi_(player|app)'
}
# A previous run that is still going holds the memory the new one needs.
waited=0
while running; do
    [ "$waited" -ge 900 ] && { echo "a previous run is still going" >&2; exit 1; }
    [ "$waited" -eq 0 ] && echo "waiting for the previous run to finish..."
    sleep 10; waited=$((waited + 10))
done
rm -f "$OUT"/*.txt.tmp "$OUT"/shot-*.bmp "$OUT/$NAME.txt" \
    "$OUT/tilefinch-validation.txt" "$OUT/tilefinch-crash.txt"
"$PROJECT_ROOT/scripts/psplink.sh" exec "ld host0:/$NAME.prx"
started=$(date +%s)
sleep 5
while running; do
    if [ $(( $(date +%s) - started )) -ge "$LIMIT_SECONDS" ]; then
        echo "still running after ${LIMIT_SECONDS}s; collecting anyway" >&2
        break
    fi
    sleep 5
done
dest="$PROJECT_ROOT/field-logs/$NAME-$(date +%Y%m%d-%H%M%S)"
mkdir -p "$dest"
for f in "$OUT/$NAME.txt" "$OUT/tilefinch-validation.txt" \
         "$OUT/tilefinch-crash.txt" "$OUT"/shot-*.bmp; do
    [ -f "$f" ] && cp "$f" "$dest/"
done
echo "logs: $dest"
cat "$dest/$NAME.txt" 2>/dev/null || true
