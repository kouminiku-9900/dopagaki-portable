#!/bin/sh
# Build the native programs (native/native.cmake) in the same tree as the
# browser EBOOT.   scripts/build_native.sh [komi-player|komi-app ...]
# Output: vendor/tilefinch/build-preset-psp/<name>/{<name>.prx,EBOOT.PBP,...}
set -eu
PROJECT_ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
. "$PROJECT_ROOT/scripts/env.sh"
cd "$PROJECT_ROOT/vendor/tilefinch"
cmake --preset psp -DPython3_EXECUTABLE="$PROJECT_ROOT/.venv/bin/python" \
    -DCMAKE_AR="$PSPDEV/bin/psp-ar" -DCMAKE_RANLIB="$PSPDEV/bin/psp-ranlib" \
    -DCMAKE_NM="$PSPDEV/bin/psp-nm" -DCMAKE_STRIP="$PSPDEV/bin/psp-strip" \
    -DCMAKE_LINKER="$PSPDEV/bin/psp-ld" -DCMAKE_OBJDUMP="$PSPDEV/bin/psp-objdump" \
    -DCMAKE_C_FLAGS_MINSIZEREL="-Os -DNDEBUG" \
    -DKOMI_EXTRA_CMAKE="$PROJECT_ROOT/native/native.cmake" > /dev/null
[ $# -gt 0 ] || set -- komi-player komi-app
targets=
for name in "$@"; do targets="$targets $name-prx $name-elf"; done
cmake --build build-preset-psp --target $targets -j8
for name in "$@"; do
    echo "$name: $PROJECT_ROOT/vendor/tilefinch/build-preset-psp/$name"
done
