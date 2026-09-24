#!/usr/bin/env python3
"""Package the PSP YouTube client with a direct Wi-Fi boot profile."""
import hashlib
import json
from pathlib import Path
import shutil
import struct
import tempfile
import time
import zipfile

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / 'vendor/tilefinch/build-preset-psp/tilefinch-install/Tilefinch'
DIST = ROOT / 'dist'
NAME = 'KOMI_TUBE'

# MEMSIZE=1 is the only value that unlocks the extra ~28 MB on the tested
# PSP-3000 CFW; the SDK default of 2 leaves the app at 22.6 MB
# (docs/field-results/2026-09-24-memprobe-v1 and -v2).
MEMSIZE = 1

def set_pbp_title(path: Path, title: str | None, icon: bytes | None, memsize: int = MEMSIZE):
    """Set MEMSIZE, and optionally the public title and the upstream launcher icon."""
    data = path.read_bytes()
    magic, version, *offsets = struct.unpack_from('<10I', data)
    if magic != 0x50425000 or len(offsets) != 8:
        raise ValueError('Invalid PBP')
    ends = offsets[1:] + [len(data)]
    parts = [data[a:b] for a, b in zip(offsets, ends)]
    sfo = parts[0]
    smagic, sversion, key_start, value_start, count = struct.unpack_from('<5I', sfo)
    if smagic != 0x46535000 or count > 128:
        raise ValueError('Invalid PARAM.SFO')
    records, keys, values = [], bytearray(), bytearray()
    found = False
    memsize_found = False
    for i in range(count):
        key_offset, kind, length, capacity, value_offset = struct.unpack_from('<HHIII', sfo, 20 + i * 16)
        key = sfo[key_start + key_offset:].split(b'\0', 1)[0]
        value = sfo[value_start + value_offset:value_start + value_offset + length]
        if key == b'TITLE':
            if title is not None:
                value = title.encode('utf-8') + b'\0'
            found = True
        elif key == b'MEMSIZE':
            if kind != 0x0404 or length != 4:
                raise ValueError('Unexpected MEMSIZE entry in PARAM.SFO')
            value = struct.pack('<I', memsize)
            memsize_found = True
        size = (max(len(value), capacity) + 3) & ~3
        records.append(struct.pack('<HHIII', len(keys), kind, len(value), size, len(values)))
        keys.extend(key + b'\0')
        values.extend(value + bytes(size - len(value)))
    if title is not None and not found:
        raise ValueError('No TITLE in PARAM.SFO')
    if not memsize_found:
        raise ValueError('No MEMSIZE in PARAM.SFO')
    ks = 20 + 16 * count
    vs = (ks + len(keys) + 3) & ~3
    parts[0] = struct.pack('<5I', smagic, sversion, ks, vs, count) + b''.join(records) + keys + bytes(vs - ks - len(keys)) + values
    # ICON0.PNG (144x80) shown in the XMB; see scripts/make_icon.py.
    if icon is not None:
        parts[1] = icon
    new_offsets, pos = [], 40
    for part in parts:
        new_offsets.append(pos)
        pos += len(part)
    path.write_bytes(struct.pack('<10I', magic, version, *new_offsets) + b''.join(parts))
    # Repacking must not change any section except the metadata section.
    result = path.read_bytes()
    bounds = new_offsets[1:] + [len(result)]
    assert all(result[a:b] == parts[i] for i, (a, b) in enumerate(zip(new_offsets, bounds)))

def main():
    if not (SOURCE / 'EBOOT.PBP').is_file():
        raise SystemExit('Build tilefinch-psp-install-tree before packaging')
    DIST.mkdir(exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='package-', dir=DIST) as temp:
        staging = Path(temp)
        app = staging / 'PSP/GAME' / NAME
        shutil.copytree(SOURCE, app)
        shutil.copy2(ROOT / 'config/boot.cfg', app / 'data/boot.cfg')
        shutil.copy2(ROOT / 'config/profile.cfg', app / 'data/profile.cfg')
        set_pbp_title(app / 'EBOOT.PBP', 'komi-tube',
                      (ROOT / 'assets/ICON0.PNG').read_bytes())
        # The launcher LoadExecs slot-a, and CFW sizes the partition from the
        # EBOOT it starts, so the browser itself needs MEMSIZE=1 as well.
        set_pbp_title(app / 'slot-a/EBOOT.PBP', None, None)
        (app / 'BUILD-INFO.json').write_text(json.dumps({
            'name': 'komi-tube',
            'engine': 'native PSP browser and media client',
            'runtime': 'PSP -> Wi-Fi -> YouTube / googlevideo.com, no companion server',
            'engine_modified': True,
            'hardware_verified_here': False,
        }, indent=2) + '\n')
        if (ROOT / 'README.md').exists():
            shutil.copy2(ROOT / 'README.md', app / 'README-ja.md')
        manifest = []
        for file in sorted(app.rglob('*')):
            if file.is_file():
                digest = hashlib.sha256(file.read_bytes()).hexdigest()
                manifest.append(f'{digest}  {file.relative_to(app).as_posix()}')
        (app / 'SHA256SUMS').write_text('\n'.join(manifest) + '\n')
        archive = staging / 'komi-tube.zip'
        with zipfile.ZipFile(archive, 'w', compression=zipfile.ZIP_DEFLATED) as z:
            for file in sorted(app.rglob('*')):
                if file.is_file():
                    z.write(file, file.relative_to(staging))
        destination = DIST / 'PSP/GAME' / NAME
        if destination.exists():
            # Never erase an installed app's user data. Keep the previous generated package.
            backup = DIST / 'previous-package'
            if backup.exists():
                backup = DIST / time.strftime('previous-package-%Y%m%d-%H%M%S')
            if backup.exists():
                raise SystemExit(f'{backup} exists; move it before repackaging')
            destination.rename(backup)
            print(f'Previous package kept at: {backup}')
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.move(str(app), destination)
        archive.replace(DIST / archive.name)
        print(f'PSP install directory: {destination}')
        print(f'ZIP: {DIST / archive.name}')

if __name__ == '__main__':
    main()
