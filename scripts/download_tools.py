#!/usr/bin/env python3
"""Fetch pinned official releases and verify SHA-256 before extraction."""
import hashlib
import json
from pathlib import Path
import subprocess
import tarfile
import zipfile

ROOT = Path(__file__).resolve().parents[1]
lock = json.loads((ROOT / 'sources.lock.json').read_text())
downloads = ROOT / 'tools/downloads'
downloads.mkdir(parents=True, exist_ok=True)
for name, archive_name in [('pspdev', 'pspdev.tar.gz'), ('ppsspp', 'ppsspp.zip')]:
    archive = downloads / archive_name
    info = lock[name]
    if not archive.exists():
        partial = archive.with_suffix('.part')
        subprocess.run(['curl', '-fL', '--retry', '3', '--proto', '=https',
                        info['url'], '-o', str(partial)], check=True)
        partial.replace(archive)
    if hashlib.file_digest(archive.open('rb'), 'sha256').hexdigest() != info['sha256']:
        raise SystemExit(f'{archive}: SHA-256 mismatch; extraction refused')
    if name == 'pspdev' and not (ROOT / 'tools/pspdev/bin/psp-gcc').exists():
        with tarfile.open(archive) as tar:
            tar.extractall(ROOT / 'tools', filter='data')
    if name == 'ppsspp' and not (ROOT / 'tools/ppsspp/PPSSPPSDL.app').exists():
        # ditto preserves the executable permissions and symlinks in macOS bundles.
        with zipfile.ZipFile(archive) as z:
            for member in z.namelist():
                if member.startswith('/') or '..' in Path(member).parts:
                    raise SystemExit('Unsafe archive member')
        subprocess.run(['ditto', '-xk', str(archive), str(ROOT / 'tools/ppsspp')], check=True)
    print(f'{name}: verified {info["version"]}')
