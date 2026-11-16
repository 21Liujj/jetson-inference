#!/usr/bin/env python3
"""Build an unchanged matched UVC module in a new owned artifact directory.

Does not install, unload, load, sign, alter kernel headers or change the kernel.
"""
import argparse
import gzip
import hashlib
import json
import os
import pathlib
import shutil
import subprocess
import sys
import zipfile


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def command(argv):
    p = subprocess.run(argv, text=True, capture_output=True)
    return {'argv': [str(x) for x in argv], 'exit_code': p.returncode, 'stdout': p.stdout, 'stderr': p.stderr}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--package', required=True, type=pathlib.Path)
    parser.add_argument('--sha256', required=True)
    parser.add_argument('--out', required=True, type=pathlib.Path)
    args = parser.parse_args()
    if sys.platform != 'linux' or os.uname().machine != 'aarch64':
        parser.error('Native Jetson/aarch64 build only')
    if sha(args.package) != args.sha256:
        raise RuntimeError('source package hash mismatch')
    root = pathlib.Path(__file__).resolve().parents[3]
    args.out = args.out.resolve()
    args.out.relative_to(root / 'projects/edgevision/artifacts')
    args.out.mkdir(parents=True, exist_ok=False)
    release = os.uname().release
    headers = (pathlib.Path('/lib/modules') / release / 'build').resolve()
    config = headers / '.config'
    symvers = headers / 'Module.symvers'
    if not config.is_file() or not symvers.is_file():
        raise RuntimeError('matching config and Module.symvers are required')
    original_config, original_symvers = sha(config), sha(symvers)
    original_module = pathlib.Path(subprocess.check_output(['modinfo', '-n', 'uvcvideo'], text=True).strip())
    record = {'release': release, 'headers': str(headers), 'config_sha256': original_config,
              'symvers_sha256': original_symvers, 'package_sha256': args.sha256,
              'compiler': command(['gcc', '--version']), 'running_kernel': pathlib.Path('/proc/version').read_text(),
              'original_module_path': str(original_module), 'original_module_sha256': sha(original_module),
              'original_modinfo': command(['modinfo', str(original_module)]), 'installed': False}
    with zipfile.ZipFile(args.package) as package:
        manifest = json.loads(package.read('uvc-sources.json'))
        record['source'] = manifest
        for entry in manifest['files']:
            name = entry['name']
            if pathlib.PurePosixPath(name).name != name or name in ('.', '..'):
                raise RuntimeError('invalid source member')
            data = package.read('uvc-pristine/' + name)
            if hashlib.sha256(data).hexdigest() != entry['sha256']:
                raise RuntimeError('source member hash mismatch')
            (args.out / name).write_bytes(data)
    argv = ['make', '-C', str(headers), 'M=' + str(args.out), 'ARCH=arm64', 'CROSS_COMPILE=', 'CC=gcc', '-j2', 'modules']
    with (args.out / 'build.log').open('xb') as output:
        process = subprocess.run(argv, stdout=output, stderr=subprocess.STDOUT)
    record['build_argv'], record['build_exit_code'] = argv, process.returncode
    record['headers_unchanged'] = original_config == sha(config) and original_symvers == sha(symvers)
    module = args.out / 'uvcvideo.ko'
    if process.returncode == 0 and module.is_file():
        record['built_module_sha256'] = sha(module)
        record['built_modinfo'] = command(['modinfo', str(module)])
        for field in ['vermagic', 'srcversion']:
            old = subprocess.check_output(['modinfo', '-F', field, str(original_module)], text=True).strip()
            new = subprocess.check_output(['modinfo', '-F', field, str(module)], text=True).strip()
            record[field] = {'original': old, 'built': new, 'equal': old == new}
        def versions(path):
            result = command(['modprobe', '--show-modversions', str(path)])
            if result['exit_code']:
                raise RuntimeError('modversions could not be read: ' + result['stderr'])
            return {line.split()[1]: line.split()[0] for line in result['stdout'].splitlines()}
        old_versions, new_versions = versions(original_module), versions(module)
        record['imported_crc'] = {'original_count': len(old_versions), 'built_count': len(new_versions),
                                 'mismatches': {name: {'original': old_versions.get(name), 'built': crc}
                                                for name, crc in new_versions.items() if old_versions.get(name) != crc}}
    (args.out / 'build-record.json').write_text(json.dumps(record, indent=2) + '\n', encoding='utf-8')
    print(json.dumps({'build_exit_code': process.returncode, 'headers_unchanged': record['headers_unchanged'],
                      'vermagic': record.get('vermagic'), 'srcversion': record.get('srcversion'),
                      'imported_crc': record.get('imported_crc'), 'record': str(args.out / 'build-record.json')}))
    return process.returncode or (0 if record['headers_unchanged'] else 1)


if __name__ == '__main__':
    raise SystemExit(main())
