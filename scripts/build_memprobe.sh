#!/bin/sh
# Build the stage-0 memory probe in three MEMSIZE variants and stage them as
# dist/field-kit/PSP/GAME/KOMI_PROBE_M{0,1,2}. Works on macOS (tools/pspdev
# from bootstrap.sh) and Linux (set PSPDEV to an extracted pspdev release).
set -eu
PROJECT_ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
if [ -z "${PSPDEV:-}" ] || [ ! -x "$PSPDEV/bin/psp-gcc" ]; then
    . "$PROJECT_ROOT/scripts/env.sh"
fi
export PATH="$PSPDEV/bin:$PATH"
command -v psp-gcc >/dev/null || { echo "psp-gcc not found; set PSPDEV" >&2; exit 1; }
PY=python3
[ -x "$PROJECT_ROOT/.venv/bin/python" ] && PY="$PROJECT_ROOT/.venv/bin/python"

SRC="$PROJECT_ROOT/native/memprobe"
KIT="$PROJECT_ROOT/dist/field-kit/PSP/GAME"
mkdir -p "$KIT"
for memsize in 0 1 2; do
    variant="m$memsize"
    out="$PROJECT_ROOT/build/memprobe/$variant"
    mkdir -p "$out"
    "$PY" "$SRC/gen_aac_fixture.py" \
        "$PROJECT_ROOT/vendor/tilefinch/tests/fixtures/psp-media/baseline-320x240.mp4" \
        "$out/aac_fixture.h"
    make -s -C "$out" -f "$SRC/Makefile" SRCDIR="$SRC" VARIANT="$variant" MEMSIZE="$memsize"
    dest="$KIT/KOMI_PROBE_M$memsize"
    mkdir -p "$dest"
    cp "$out/EBOOT.PBP" "$dest/EBOOT.PBP"
    echo "staged $dest/EBOOT.PBP"
done
