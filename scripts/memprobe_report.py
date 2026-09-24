#!/usr/bin/env python3
"""Turn memprobe-*.txt logs into the stage-0 memory table (Markdown).

Usage: scripts/memprobe_report.py <log or directory> [...]
Reads the last complete run in each file; a run cut short (hang) is reported
with the step it stopped at.
"""
import re
import sys
from pathlib import Path

LIMIT = 0x0A000000


def fields(line):
    return dict(re.findall(r'(\w+)=(\S+)', line))


def last_run(text):
    runs = text.split('==== run begin')
    return '==== run begin' + runs[-1] if len(runs) > 1 else ''


def mb(value):
    return f'{int(value) / (1024 * 1024):.2f} MB'


def summarize(path):
    run = last_run(path.read_text(errors='replace'))
    lines = run.splitlines()
    if not lines:
        return None
    head = fields(lines[0])
    info = {'file': path.name, 'variant': head.get('variant', '?'),
            'complete': any(l.startswith('==== run end') for l in lines),
            'last_line': lines[-1], 'mem': {}, 'maps': {}, 'codec': {},
            'modules': []}
    for line in lines:
        kind = line.split(' ', 1)[0]
        f = fields(line)
        if kind == 'device':
            info['devkit'], info['model'] = f['devkit'], f['model']
        elif kind == 'mem':
            info['mem'][f['label']] = (int(f['total_free']), int(f['max_free']))
        elif kind == 'map':
            info['maps'].setdefault(f['label'], []).append(
                (int(f['start'], 16), int(f['end'], 16)))
        elif kind == 'map-total':
            info['maps'][f['label'] + '#total'] = f
        elif kind == 'me-region':
            info['me'] = f
        elif kind == 'arena':
            info['arena'] = f
        elif kind == 'codec-result':
            info['codec'][f['label']] = f
        elif kind == 'codec-fail':
            info['codec'][f['label']] = f
        elif kind == 'volatile':
            info['volatile'] = f
        elif kind == 'komi-tube-eboot':
            info['komi_memsize'] = f.get('memsize', f.get('status'))
        elif kind == 'cycles':
            info['cycles'] = f
        elif kind in ('module', 'net'):
            info['modules'].append(line)
    return info


def codec_state(info, label):
    f = info['codec'].get(label)
    if f is None:
        return 'not run'
    if 'stage' in f:
        return f"FAIL at {f['stage']} ({f['status']})"
    ok = f['ok']
    reference = info['codec'].get('me-region', {}).get('checksum')
    same = f.get('checksum') == reference
    good = ok.split('/')[0] == ok.split('/')[1] and int(f['pcm_changed']) > 0
    if good and (same or label == 'me-region'):
        return f'ok ({ok})'
    return f"FAIL ({ok}, err={f['first_error']}, pcm-changed={f['pcm_changed']}, same-pcm={same})"


def report(info):
    out = [f"## {info['file']} (variant {info['variant']})", '']
    if not info['complete']:
        out += [f"**Run did not finish.** Last line: `{info['last_line']}`", '']
    out.append(f"- devkit {info.get('devkit', '?')}, kuKernelGetModel {info.get('model', '?')}")
    if 'komi_memsize' in info:
        out.append(f"- installed komi-tube EBOOT MEMSIZE: {info['komi_memsize']} (-1 = key absent)")
    for label, (total, largest) in info['mem'].items():
        out.append(f'- free `{label}`: total {mb(total)}, largest {mb(largest)}')
    for label in ('boot', 'after-modules', 'after-layout'):
        total = info['maps'].get(label + '#total')
        if total:
            out.append(f"- free map `{label}`: {total['ranges']} ranges, "
                       f"below 0x0A000000 {mb(total['free_below'])}, "
                       f"above {mb(total['free_above'])}")
            for start, end in info['maps'].get(label, []):
                out.append(f'  - 0x{start:08X}-0x{end:08X} ({mb(end - start)})')
    me, arena = info.get('me'), info.get('arena')
    if me:
        out.append(f"- ME region: {me['start']}-{me['end']} ({mb(me['size'])}, "
                   f"{me['mode']}, {me['side']} the limit)")
    if arena:
        out.append(f"- arena: {arena['start']}-{arena['end']} ({mb(arena['size'])}, "
                   f"{mb(arena['above_limit'])} above the limit)")
    vol = info.get('volatile')
    if vol:
        out.append(f"- volatile memory lock {vol['lock']} at {vol['ptr']} size {vol['size']}")
    out.append('- Media Engine reads (AAC decode):')
    for label in ('me-region', 'volatile', 'mixed-io-high', 'arena-high'):
        out.append(f'  - {label}: {codec_state(info, label)}')
    cycles = info.get('cycles')
    if cycles:
        leak = int(cycles['free_before']) - int(cycles['free_after'])
        out.append(f"- 10 codec cycles: {cycles['ok']}, partition change {leak} bytes")
    out.append('- module / net init statuses:')
    out += [f'  - `{line}`' for line in info['modules']]
    out.append('')
    return out


def main():
    paths = []
    for arg in sys.argv[1:] or ['.']:
        p = Path(arg)
        paths += sorted(p.rglob('*memprobe-*.txt')) if p.is_dir() else [p]
    if not paths:
        raise SystemExit('no memprobe-*.txt found')
    lines = ['# memprobe report', '']
    for path in paths:
        info = summarize(path)
        lines += report(info) if info else [f'## {path.name}: empty', '']
    print('\n'.join(lines))


if __name__ == '__main__':
    main()
