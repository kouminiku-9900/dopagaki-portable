#!/bin/sh
# Build and put dopagaki-portable on the PSP as PSP/GAME/DOPAGAKI. Uses
# PSPLink when it is connected, otherwise the mounted Memory Stick
# ($PSP_VOLUME, default "/Volumes/NO NAME").
# dopagaki-mylist.txt and komi-wifi.txt on the PSP are left alone.
set -eu
PROJECT_ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
"$PROJECT_ROOT/scripts/build_native.sh" dopagaki > /dev/null
"$PROJECT_ROOT/.venv/bin/python" "$PROJECT_ROOT/scripts/package_dopagaki.py" > /dev/null
SRC="$PROJECT_ROOT/dist/PSP/GAME/DOPAGAKI"
FILES="EBOOT.PBP roots.pem fonts/TilefinchSans-Regular.ttf fonts/LICENSE-TilefinchSans.txt fonts/LICENSE-Unifont.txt"
VOLUME=${PSP_VOLUME:-"/Volumes/NO NAME"}
if [ -d "$VOLUME/PSP/GAME" ]; then
    dest="$VOLUME/PSP/GAME/DOPAGAKI"
    mkdir -p "$dest/fonts"
    for f in $FILES; do COPYFILE_DISABLE=1 cp "$SRC/$f" "$dest/$f"; cmp "$SRC/$f" "$dest/$f"; done
    find "$dest" -name '._*' -delete
    sync
    echo "installed to $dest (eject the stick before unplugging)"
    exit 0
fi
export HOST_ROOT="$PROJECT_ROOT/dist"
"$PROJECT_ROOT/scripts/psplink.sh" ready > /dev/null
sh_exec() { LINK_TIMEOUT_SECONDS=60 "$PROJECT_ROOT/scripts/psplink.sh" exec "$1"; }
sh_exec 'mkdir ms0:/PSP/GAME/DOPAGAKI' > /dev/null 2>&1 || true
sh_exec 'mkdir ms0:/PSP/GAME/DOPAGAKI/fonts' > /dev/null 2>&1 || true
for f in $FILES; do
    sh_exec "cp host0:/PSP/GAME/DOPAGAKI/$f ms0:/PSP/GAME/DOPAGAKI/$f" > /dev/null
done
# Read the program back and compare, so a short copy cannot pass silently.
sh_exec "cp ms0:/PSP/GAME/DOPAGAKI/EBOOT.PBP host0:/verify-dopagaki-eboot.pbp" > /dev/null
cmp "$PROJECT_ROOT/dist/verify-dopagaki-eboot.pbp" "$SRC/EBOOT.PBP"
rm -f "$PROJECT_ROOT/dist/verify-dopagaki-eboot.pbp"
echo "installed to ms0:/PSP/GAME/DOPAGAKI over PSPLink (verified)"
