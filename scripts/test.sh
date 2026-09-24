#!/bin/sh
set -eu
PROJECT_ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
. "$PROJECT_ROOT/scripts/env.sh"
cd "$PROJECT_ROOT/vendor/tilefinch"
cmake --preset release -DPython3_EXECUTABLE="$PROJECT_ROOT/.venv/bin/python"
cmake --build build-preset-release -j8
ctest --test-dir build-preset-release -j8 --output-on-failure
