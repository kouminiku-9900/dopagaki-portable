#!/usr/bin/env python3
"""Stage dopagaki-portable for the Memory Stick: dist/PSP/GAME/DOPAGAKI,
and dist/dopagaki-portable.zip.

Program files only (EBOOT.PBP with MEMSIZE=1, roots.pem, fonts/) plus the
redistribution notices. The files the app writes beside itself --
dopagaki-mylist.txt, komi-wifi.txt -- are never part of the package, so
installing over an old copy keeps them.
"""
from pathlib import Path
import shutil
import sys
import zipfile

sys.path.insert(0, str(Path(__file__).resolve().parent))
from package import ROOT, set_pbp_title  # noqa: E402

TF = ROOT / 'vendor/tilefinch'
SOURCE = TF / 'build-preset-psp/dopagaki'
DEST = ROOT / 'dist/PSP/GAME/DOPAGAKI'
TITLE = 'dopagaki-portable'
FILES = ['EBOOT.PBP', 'roots.pem', 'fonts/TilefinchSans-Regular.ttf',
         'fonts/LICENSE-TilefinchSans.txt', 'fonts/LICENSE-Unifont.txt']


def stage_notices(notices: Path):
    """The notice set tilefinch's StagePspInstall.cmake stages for the same
    linked components (its THIRD_PARTY_NOTICES.md checklist)."""
    notices.mkdir(parents=True)
    shutil.copy2(ROOT / 'LICENSE', notices / 'LICENSE-dopagaki-portable')
    shutil.copy2(TF / 'LICENSE', notices / 'LICENSE')
    shutil.copy2(TF / 'THIRD_PARTY_NOTICES.md',
                 notices / 'THIRD_PARTY_NOTICES.md')
    for entry in sorted((TF / 'third_party/notices').iterdir()):
        target = notices / entry.name
        if entry.is_dir():
            shutil.copytree(entry, target)
        else:
            shutil.copy2(entry, target)
    shutil.copytree(TF / 'third_party/public_suffix',
                    notices / 'public_suffix')
    (notices / 'pocketsphinx').mkdir()
    shutil.copy2(TF / 'third_party/POCKETSPHINX_LICENSE.txt',
                 notices / 'pocketsphinx/POCKETSPHINX_LICENSE.txt')
    (notices / 'voice-model').mkdir()
    shutil.copy2(TF / 'psp-assets/voice-model/en-us/README',
                 notices / 'voice-model/ALPHA_CEPHEI_LICENSE.txt')
    (notices / 'fonts').mkdir()
    for name in ('LICENSE-DejaVu.txt', 'LICENSE-TilefinchSans.txt',
                 'LICENSE-Unifont.txt'):
        shutil.copy2(TF / 'fonts' / name, notices / 'fonts' / name)


def main():
    if DEST.exists():
        shutil.rmtree(DEST)
    for name in FILES:
        target = DEST / name
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(SOURCE / name, target)
    set_pbp_title(DEST / 'EBOOT.PBP', TITLE,
                  (ROOT / 'assets/ICON0-dopagaki.png').read_bytes())
    stage_notices(DEST / 'NOTICES')
    shutil.copy2(ROOT / 'README.md', DEST / 'README-ja.md')
    archive = ROOT / 'dist/dopagaki-portable.zip'
    with zipfile.ZipFile(archive, 'w', compression=zipfile.ZIP_DEFLATED) as z:
        for file in sorted(DEST.rglob('*')):
            if file.is_file():
                z.write(file, file.relative_to(ROOT / 'dist'))
    print(f'ZIP: {archive}')


if __name__ == '__main__':
    main()
