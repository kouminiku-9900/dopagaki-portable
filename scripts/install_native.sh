#!/bin/sh
# Build and put the native client on the PSP as PSP/GAME/KOMI_NATIVE
# ("komi-tube（新）" in the XMB). Uses PSPLink when it is connected, otherwise
# the mounted Memory Stick ($PSP_VOLUME, default "/Volumes/NO NAME").
# komi-mylist.txt and komi-wifi.txt on the PSP are left alone.
set -eu
PROJECT_ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
"$PROJECT_ROOT/scripts/build_native.sh" komi-app > /dev/null
"$PROJECT_ROOT/.venv/bin/python" "$PROJECT_ROOT/scripts/package_native.py" > /dev/null
SRC="$PROJECT_ROOT/dist/PSP/GAME/KOMI_NATIVE"
FILES="EBOOT.PBP roots.pem fonts/TilefinchSans-Regular.ttf fonts/LICENSE-TilefinchSans.txt fonts/LICENSE-Unifont.txt"
VOLUME=${PSP_VOLUME:-"/Volumes/NO NAME"}
if [ -d "$VOLUME/PSP/GAME" ]; then
    dest="$VOLUME/PSP/GAME/KOMI_NATIVE"
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
sh_exec 'mkdir ms0:/PSP/GAME/KOMI_NATIVE' > /dev/null 2>&1 || true
sh_exec 'mkdir ms0:/PSP/GAME/KOMI_NATIVE/fonts' > /dev/null 2>&1 || true
for f in $FILES; do
    sh_exec "cp host0:/PSP/GAME/KOMI_NATIVE/$f ms0:/PSP/GAME/KOMI_NATIVE/$f" > /dev/null
done
# Read the program back and compare, so a short copy cannot pass silently.
sh_exec "cp ms0:/PSP/GAME/KOMI_NATIVE/EBOOT.PBP host0:/verify-native-eboot.pbp" > /dev/null
cmp "$PROJECT_ROOT/dist/verify-native-eboot.pbp" "$SRC/EBOOT.PBP"
rm -f "$PROJECT_ROOT/dist/verify-native-eboot.pbp"
echo "installed to ms0:/PSP/GAME/KOMI_NATIVE over PSPLink (verified)"
