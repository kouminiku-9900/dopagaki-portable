#!/usr/bin/env python3
"""Install PSPDEV's macOS compiler libraries inside the project, not Homebrew."""
import hashlib
import json
import pathlib
import shutil
import subprocess
import tarfile

ROOT = pathlib.Path(__file__).resolve().parents[1]
PACKAGES = {
    'pkgconf': '3394224fbc510e07acb45f3ad41d62857cfefe2394e8f71ca2e976b59864d5e6',
    'zstd': 'd72adf48460a8384b256f88061cd7b9df4977df7fa2e0794051d427db754a565',
    'gmp': 'b30bf31c50c294e0981d2a7cb4c149e9c43f50f3cf4f7f552c3dcf9da66b95b5',
    'mpfr': 'ed822b7e77645d7c17abb3ee9cc2b2a82a4d0f003acc7615b5df6226031479b2',
    'libmpc': 'e7723a06cf55d69322ada010ad25c6b34627674729e41d89f2526edfa7ba6995',
}

def run(*args):
    return subprocess.check_output(args)

def main():
    output = ROOT / 'tools/host-libs'
    output.mkdir(parents=True, exist_ok=True)
    for name, digest in PACKAGES.items():
        archive = ROOT / f'tools/downloads/{name}.bottle.tar.gz'
        if not archive.exists():
            token = json.loads(run('curl', '-fsSL',
                f'https://ghcr.io/token?service=ghcr.io&scope=repository:homebrew/core/{name}:pull'))['token']
            subprocess.run(['curl', '-fsSL', '--retry', '3', '-H', f'Authorization: Bearer {token}',
                f'https://ghcr.io/v2/homebrew/core/{name}/blobs/sha256:{digest}', '-o', str(archive)], check=True)
        assert hashlib.sha256(archive.read_bytes()).hexdigest() == digest, f'Hash mismatch: {name}'
        unpack = ROOT / f'tools/host-packages/{name}'
        unpack.mkdir(parents=True, exist_ok=True)
        with tarfile.open(archive) as tar:
            tar.extractall(unpack, filter='data')
        for lib in unpack.rglob('*.dylib'):
            if lib.is_file():
                shutil.copy2(lib, output / lib.name)
    host_bin = ROOT / 'tools/host-bin'
    host_bin.mkdir(exist_ok=True)
    for executable in (ROOT / 'tools/host-packages/pkgconf').rglob('bin/pkgconf'):
        shutil.copy2(executable, host_bin / 'pkgconf')
    alias = host_bin / 'pkg-config'
    if not alias.exists():
        alias.symlink_to('pkgconf')
    binaries = list(output.glob('*.dylib')) + [host_bin / 'pkgconf']
    sdk = ROOT / 'tools/pspdev'
    binaries += [p for p in (sdk / 'libexec/gcc/psp/15.2.0').iterdir()
                 if p.is_file() and p.name in ('cc1', 'cc1plus', 'lto1', 'collect2', 'lto-wrapper')]
    binaries += list((sdk / 'bin').glob('psp-*'))
    binaries += list((sdk / 'psp/bin').iterdir())
    binaries = sorted({p.resolve() for p in binaries if p.is_file()})
    for binary in binaries:
        try:
            linked = run('otool', '-L', str(binary)).decode()
        except subprocess.CalledProcessError:
            continue
        changed = False
        for line in linked.splitlines()[1:]:
            source = line.strip().split(' (')[0]
            target = output / pathlib.Path(source).name
            if target.exists():
                local = binary.parent / target.name
                if local != target and not local.exists():
                    local.symlink_to(__import__('os').path.relpath(target, binary.parent))
                replacement = '@loader_path/' + target.name
                if source == replacement:
                    continue
                subprocess.run(['install_name_tool', '-change', source, replacement, str(binary)], check=True)
                changed = True
        if changed:
            subprocess.run(['codesign', '--force', '--sign', '-', str(binary)], check=True, capture_output=True)
    print(f'Compiler libraries: {output}')

if __name__ == '__main__':
    main()
