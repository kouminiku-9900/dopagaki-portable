#!/bin/sh
# PSPLink over USB, via tilefinch's bounded wrapper
# (vendor/tilefinch/docs/engineering/PSPLINK_DEV_LOOP.md).
#   scripts/psplink.sh ready                 start usbhostfs_pc if needed, wait for the PSP
#   scripts/psplink.sh exec 'ls host0:/'     run one pspsh command with a timeout
# host0: is dist/ unless HOST_ROOT is set. The PSP side is ms0:/PSP/GAME/PSPLINK,
# built by vendor/tilefinch/scripts/build-psplink-home-exit.sh (HOME returns to XMB,
# scrshot is rewritten to the media-safe scrshot-user).
set -eu
PROJECT_ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
. "$PROJECT_ROOT/scripts/env.sh"
export PSPDEV
export HOST_ROOT=${HOST_ROOT:-$PROJECT_ROOT/dist}
exec "$PROJECT_ROOT/vendor/tilefinch/scripts/psplink-shell.sh" "$@"
