#!/bin/sh
# One command each way for a device session (docs/FIELD-TEST.md).
#   scripts/field_sync.sh install   copy komi-tube (never its data/) and the
#                                   probes from dist/ onto the memory stick
#   scripts/field_sync.sh collect   copy every log back to field-logs/<time>/,
#                                   retire them on the stick, print the report
# The stick is found at $PSP_VOLUME (default "/Volumes/NO NAME").
set -eu
PROJECT_ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
VOLUME=${PSP_VOLUME:-"/Volumes/NO NAME"}
GAME="$VOLUME/PSP/GAME"
[ -d "$GAME" ] || { echo "PSP memory stick not found at $VOLUME (set PSP_VOLUME)" >&2; exit 1; }

install_tree() {
    # $1 source dir, $2 destination dir; data/ on the stick is left alone.
    mkdir -p "$2"
    COPYFILE_DISABLE=1 rsync -r --inplace --exclude 'data/' "$1/" "$2/"
    if [ -d "$1/data" ] && [ ! -d "$2/data" ]; then
        COPYFILE_DISABLE=1 rsync -r --inplace "$1/data/" "$2/data/"
    fi
    find "$2" -name '._*' -delete
    bad=$( cd "$1" && find . -type f ! -path './data/*' | while read -r f; do
        cmp -s "$1/$f" "$2/$f" || echo "$2/$f"
    done )
    if [ -n "$bad" ]; then
        echo "verify failed:" >&2
        echo "$bad" >&2
        exit 1
    fi
    echo "installed $2"
}

case "${1:-}" in
install)
    for probe in "$PROJECT_ROOT"/dist/field-kit/PSP/GAME/*; do
        [ -d "$probe" ] && install_tree "$probe" "$GAME/$(basename "$probe")"
    done
    if [ -d "$PROJECT_ROOT/dist/PSP/GAME/KOMI_TUBE" ]; then
        install_tree "$PROJECT_ROOT/dist/PSP/GAME/KOMI_TUBE" "$GAME/KOMI_TUBE"
    fi
    sync
    echo "Done. Eject the stick before removing it."
    ;;
collect)
    stamp=$(date +%Y%m%d-%H%M%S)
    out="$PROJECT_ROOT/field-logs/$stamp"
    mkdir -p "$out"
    found=0
    # Only logs: data/ also holds settings (adblock.txt, *.cfg) that stay.
    for f in "$GAME"/KOMI_PROBE_*/memprobe-*.txt "$GAME"/KOMI_TUBE/data/tilefinch-*.txt; do
        [ -f "$f" ] || continue
        case "$f" in *.old.txt) continue ;; esac
        name=$(printf '%s' "${f#"$GAME"/}" | tr '/' '-')
        cp "$f" "$out/$name"
        # Retire the copy on the stick so the next session starts clean.
        mv "$f" "${f%.txt}.$stamp.old.txt"
        found=$((found + 1))
    done
    echo "collected $found file(s) into $out"
    if ls "$out"/*memprobe-*.txt >/dev/null 2>&1; then
        python3 "$PROJECT_ROOT/scripts/memprobe_report.py" "$out" > "$out/memprobe-report.md"
        cat "$out/memprobe-report.md"
    fi
    ;;
*)
    echo "usage: $0 install|collect" >&2
    exit 2
    ;;
esac
