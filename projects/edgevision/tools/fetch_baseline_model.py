#!/usr/bin/env python3
"""Fetch and verify the upstream SSD fruit baseline without training packages."""
import argparse
import hashlib
import pathlib
import shutil
import tarfile
import tempfile
import urllib.request

URL = 'https://nvidia.box.com/shared/static/gq0zlf0g2r258g3ldabl9o7vch18cxmi.gz'
ARCHIVE_SHA256 = '88ed0965a4e487ba17a797cf7599891689d00c4170fbf056c8e608758775e483'
FILES = {
    'ssd-mobilenet.onnx': '926c60bc19e61a6f54a806cbb47acd3c890862f2907a465736362696d5b38c6c',
    'labels.txt': '9513d407a6f5e94efaf69ca6ee1e17b0b5f1f3653b7d7ef502c5927c72b8549e',
}


def digest(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest() if hasattr(hashlib, 'file_digest') else hashlib.sha256(stream.read()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--out', type=pathlib.Path, required=True)
    parser.add_argument('--archive', type=pathlib.Path, help='Use an already downloaded archive (network-free)')
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    for name, expected in FILES.items():
        path = args.out / name
        if path.exists() and digest(path) != expected:
            raise ValueError('Refusing to overwrite mismatched existing file: ' + str(path))
    if all((args.out / name).exists() for name in FILES):
        print('existing_verified: ONNX and labels match locked SHA256')
        return
    with tempfile.TemporaryDirectory(prefix='edgevision-model-') as temporary:
        archive = args.archive or pathlib.Path(temporary) / 'upstream.tar.gz'
        if args.archive is None:
            with urllib.request.urlopen(URL, timeout=45) as response, archive.open('wb') as output:
                shutil.copyfileobj(response, output)
        if digest(archive) != ARCHIVE_SHA256:
            raise ValueError('Archive SHA256 does not match the frozen upstream model')
        with tarfile.open(archive, 'r:gz') as source:
            for name, expected in FILES.items():
                target = args.out / name
                if target.exists():
                    continue
                member = source.getmember('ssd-mobilenet-fruit/' + name)
                if not member.isfile():
                    raise ValueError('Expected regular archive member: ' + name)
                staging = pathlib.Path(temporary) / name
                with source.extractfile(member) as stream, staging.open('wb') as output:
                    shutil.copyfileobj(stream, output)
                if digest(staging) != expected:
                    raise ValueError('Extracted artifact SHA256 mismatch: ' + name)
                # Exclusive creation protects existing models and avoids tar path traversal.
                with staging.open('rb') as stream, target.open('xb') as output:
                    shutil.copyfileobj(stream, output)
                print('verified: ' + str(target))


if __name__ == '__main__':
    main()
