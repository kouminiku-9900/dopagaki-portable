#!/bin/sh
set -eu
PROJECT_ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$PROJECT_ROOT"
[ "$(uname -s)-$(uname -m)" = Darwin-arm64 ] || {
    echo 'This setup script currently supports Apple Silicon macOS.' >&2
    exit 1
}
python3 -m venv .venv
.venv/bin/python -m pip install -r requirements-dev.txt
.venv/bin/python scripts/download_tools.py
.venv/bin/python scripts/local_sdk_libs.py
. "$PROJECT_ROOT/scripts/env.sh"
vendor/tilefinch/scripts/fetch-psp-transport-deps.sh
printf '\nSetup complete. Run ./scripts/build.sh\n'
