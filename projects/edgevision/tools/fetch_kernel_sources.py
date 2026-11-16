#!/usr/bin/env python3
"""Freeze the official R36.4.3 BSP source archive and complete UVC sources.

Does not execute vendor scripts, change headers, build or install a module.
Computed hashes identify downloaded bytes; they are not published vendor hashes.
"""
import argparse
import hashlib
import json
import pathlib
import shutil
import tarfile
import urllib.request

URL = 'https://developer.nvidia.com/downloads/embedded/l4t/r36_release_v4.3/sources/public_sources.tbz2'


def digest(path):
    value = hashlib.sha256()
    with path.open('rb') as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b''):
            value.update(block)
    return value.hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--out', required=True, type=pathlib.Path)
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    archive = args.out / 'public_sources.tbz2'
    metadata = args.out / 'download.json'
    if not archive.exists():
        partial = args.out / 'public_sources.tbz2.part'
        with urllib.request.urlopen(URL, timeout=60) as response, partial.open('xb') as output:
            length = int(response.headers.get('Content-Length', 0))
            if length > 2 * 1024 ** 3:
                raise RuntimeError('unexpected archive size')
            record = {'requested_url': URL, 'resolved_url': response.url,
                      'content_length': length, 'etag': response.headers.get('ETag'),
                      'last_modified': response.headers.get('Last-Modified'),
                      'checksum_authority': 'locally computed SHA256; no published source checksum located'}
            count = 0
            next_report = 32 * 1024 ** 2
            while True:
                block = response.read(1024 * 1024)
                if not block:
                    break
                output.write(block)
                count += len(block)
                if count > 2 * 1024 ** 3:
                    raise RuntimeError('download exceeded budget')
                if count >= next_report:
                    print(json.dumps({'downloaded_bytes': count}), flush=True)
                    next_report += 32 * 1024 ** 2
            if length and count != length:
                raise RuntimeError('incomplete archive; partial file retained')
        partial.rename(archive)
        record.update(bytes=count, sha256=digest(archive))
        metadata.write_text(json.dumps(record, indent=2) + '\n', encoding='utf-8')
    else:
        record = json.loads(metadata.read_text(encoding='utf-8'))
        if digest(archive) != record['sha256']:
            raise RuntimeError('cached archive hash mismatch')
    kernel = args.out / 'kernel_src.tbz2'
    if not kernel.exists():
        with tarfile.open(archive, 'r:bz2') as outer:
            entries = [entry for entry in outer if entry.isfile() and entry.name.endswith('/kernel_src.tbz2')]
            if len(entries) != 1:
                raise RuntimeError('ambiguous kernel source archive')
            with outer.extractfile(entries[0]) as source, kernel.open('xb') as output:
                shutil.copyfileobj(source, output, 1024 * 1024)
        record['kernel_archive_member'] = entries[0].name
        record['kernel_archive_sha256'] = digest(kernel)
        metadata.write_text(json.dumps(record, indent=2) + '\n', encoding='utf-8')
    elif digest(kernel) != record['kernel_archive_sha256']:
        raise RuntimeError('cached kernel archive hash mismatch')
    source_dir = args.out / 'uvc-pristine'
    source_dir.mkdir(exist_ok=True)
    manifest = []
    version = None
    with tarfile.open(kernel, 'r|bz2') as inner:
        for entry in inner:
            if not entry.isfile():
                continue
            key = '/drivers/media/usb/uvc/'
            if key in entry.name:
                name = entry.name.split(key, 1)[1]
                if '/' in name or name in ('.', '..') or entry.size > 4 * 1024 ** 2:
                    raise RuntimeError('unexpected UVC member')
                data = inner.extractfile(entry).read()
                path = source_dir / name
                if path.exists() and path.read_bytes() != data:
                    raise RuntimeError('existing UVC source differs')
                if not path.exists():
                    path.write_bytes(data)
                manifest.append({'source_member': entry.name, 'name': name, 'bytes': len(data),
                                 'sha256': hashlib.sha256(data).hexdigest()})
            if entry.name.endswith('/kernel-jammy-src/Makefile'):
                version = inner.extractfile(entry).read().decode('utf-8')[:1200]
    required = {'uvc_driver.c', 'uvc_video.c', 'uvc_queue.c', 'uvcvideo.h', 'Makefile', 'Kconfig'}
    if not required.issubset({entry['name'] for entry in manifest}):
        raise RuntimeError('incomplete UVC source set')
    (args.out / 'uvc-sources.json').write_text(json.dumps({'release': 'R36.4.3',
        'download': record, 'kernel_makefile_header': version, 'files': manifest}, indent=2) + '\n', encoding='utf-8')
    print(json.dumps({'archive_sha256': record['sha256'], 'kernel_sha256': record['kernel_archive_sha256'],
                      'uvc_files': len(manifest)}), flush=True)


if __name__ == '__main__':
    main()
