#!/usr/bin/env python3
"""Export text evidence only; never package images, models, engines or binaries."""
import argparse
import hashlib
import json
import pathlib
import re
import zipfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=pathlib.Path, required=True)
    parser.add_argument('--out', type=pathlib.Path, required=True)
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=False)
    paths = sorted(p for p in args.source.rglob('*') if p.is_file() and not p.is_symlink()
                   and p.suffix in {'.json', '.log', '.csv', '.txt', '.exit'})
    total = sum(p.stat().st_size for p in paths)
    if total > 64 * 1024 * 1024:
        raise ValueError('Evidence exceeds 64 MiB text budget')
    files = []
    with zipfile.ZipFile(args.out / 'raw-text-evidence.zip', 'w', zipfile.ZIP_DEFLATED, compresslevel=9) as archive:
        for path in paths:
            raw = path.read_bytes()
            name = path.relative_to(args.source).as_posix()
            archive.writestr(name, raw)
            files.append({'path': name, 'bytes': len(raw), 'sha256': hashlib.sha256(raw).hexdigest()})
    result = {'schema_version': 1, 'simulated': False,
              'scope': 'M0 environment and M1 upstream / compatibility experiments',
              'files': files, 'uncompressed_bytes': total,
              'archive_sha256': hashlib.sha256((args.out / 'raw-text-evidence.zip').read_bytes()).hexdigest(),
              'capture': json.loads((args.source / 'capture-summary.json').read_text()), 'runs': {}}
    for path in sorted(args.source.glob('*/run.json')):
        record = json.loads(path.read_text())
        log = (path.parent / 'stdout.log').read_text(errors='replace')
        record['output_image_calls'] = len(re.findall(r'\[image\]\s+saved ', log))
        record['inference_timing_reports'] = len(re.findall(r'Timing Report ', log))
        record['shutdown_complete'] = 'shutdown complete' in log
        # Retain the original run metadata; these observations do not by
        # themselves turn any run into an acceptance pass.
        result['runs'][path.parent.name] = record
    (args.out / 'manifest.json').write_text(json.dumps(result, indent=2) + '\n', encoding='utf-8')
    print(json.dumps({'out': str(args.out), 'files': len(files), 'uncompressed_bytes': total,
                      'archive_bytes': (args.out / 'raw-text-evidence.zip').stat().st_size}))


if __name__ == '__main__':
    main()
