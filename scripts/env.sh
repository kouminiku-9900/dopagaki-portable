#!/bin/sh
# Source from a script that has already set PROJECT_ROOT.
export PSPDEV="$PROJECT_ROOT/tools/pspdev"
export PATH="$PROJECT_ROOT/.venv/bin:$PSPDEV/bin:$PROJECT_ROOT/tools/host-bin:$PATH"
