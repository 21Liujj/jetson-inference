#!/usr/bin/env python3
"""Prepare, then temporarily verify and restore an unchanged UVC module.

No module file under /lib/modules is overwritten. No depmod, boot change,
signature-policy change, forced unload or forced load is performed.
"""
import argparse
import hashlib
import json
import os
import pathlib
import re
import shutil
import struct
import subprocess
import sys
import time


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def run(argv):
    p = subprocess.run([str(x) for x in argv], text=True, capture_output=True)
    return {'argv': [str(x) for x in argv], 'exit_code': p.returncode, 'stdout': p.stdout, 'stderr': p.stderr}


def require(ok, message):
    if not ok:
        raise RuntimeError(message)


def build_id(path):
    r = run(['readelf', '-n', path])
    ids = re.findall(r'Build ID: ([0-9a-f]+)', r['stdout'])
    require(r['exit_code'] == 0 and len(ids) == 1, 'MODULE_BUILD_ID_UNAVAILABLE')
    return ids[0]


def loaded_id():
    note = pathlib.Path('/sys/module/uvcvideo/notes/.note.gnu.build-id')
    if not note.exists():
        return None
    data = note.read_bytes()
    namesize, size, kind = struct.unpack_from('<III', data)
    require(kind == 3 and data[12:12 + namesize].rstrip(b'\0') == b'GNU', 'INVALID_MODULE_NOTE')
    offset = 12 + ((namesize + 3) & ~3)
    return data[offset:offset + size].hex()


def owners():
    nodes = list(pathlib.Path('/dev').glob('video*'))
    if not nodes:
        return ''
    r = run(['fuser', *nodes])
    require(r['exit_code'] in (0, 1), 'FUSER_FAILED: ' + r['stderr'])
    return r['stdout'].strip()


def settle(device):
    end = time.monotonic() + 10
    while time.monotonic() < end:
        if device.exists():
            return
        time.sleep(.05)
    raise RuntimeError('STABLE_CAMERA_NODE_NOT_RESTORED')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--mode', choices=['prepare', 'verify'], required=True)
    parser.add_argument('--out', required=True, type=pathlib.Path)
    parser.add_argument('--build-record', type=pathlib.Path)
    parser.add_argument('--capture-binary', type=pathlib.Path)
    parser.add_argument('--device', default='/dev/v4l/by-id/usb-Generic_Web_Camera_20250708V1.000-video-index0')
    args = parser.parse_args()
    require(sys.platform == 'linux' and os.geteuid() == 0, 'ROOT_REQUIRED_FOR_MODULE_VERIFICATION')
    root = pathlib.Path(__file__).resolve().parents[3]
    args.out = args.out.resolve()
    args.out.relative_to(root / 'projects/edgevision/artifacts')
    plan_file = args.out / 'prepared-plan.json'
    if args.mode == 'prepare':
        require(args.build_record and args.capture_binary, 'BUILD_RECORD_AND_CAPTURE_BINARY_REQUIRED')
        record = json.loads(args.build_record.read_text())
        require(record['build_exit_code'] == 0 and record['headers_unchanged'], 'UNVERIFIED_BUILD')
        require(record['vermagic']['equal'] and record['srcversion']['equal'] and not record['imported_crc']['mismatches'], 'MODULE_ABI_OR_SOURCE_MISMATCH')
        original = pathlib.Path(record['original_module_path'])
        candidate = args.build_record.resolve().parent / 'uvcvideo.ko'
        require(sha(original) == record['original_module_sha256'] and sha(candidate) == record['built_module_sha256'], 'MODULE_BYTES_CHANGED')
        require(not owners(), 'VIDEO_DEVICE_BUSY')
        original_id, candidate_id = build_id(original), build_id(candidate)
        require(loaded_id() == original_id, 'RUNNING_MODULE_IS_NOT_RECORDED_STOCK_MODULE')
        args.out.mkdir(parents=True, exist_ok=False)
        backup = args.out / 'stock-uvcvideo.ko'
        shutil.copyfile(original, backup)
        require(sha(backup) == sha(original), 'BACKUP_HASH_MISMATCH')
        rollback = args.out / 'rollback.sh'
        rollback.write_text('#!/bin/sh\nset -eu\nif [ -d /sys/module/uvcvideo ]; then rmmod uvcvideo; fi\nmodprobe uvcvideo\n')
        rollback.chmod(0o755)
        policy = pathlib.Path('/sys/module/module/parameters/sig_enforce')
        policy_value = policy.read_text().strip() if policy.exists() else None
        candidate_signer = subprocess.check_output(['modinfo', '-F', 'signer', str(candidate)], text=True).strip()
        require(policy_value != 'Y' or bool(candidate_signer), 'UNSIGNED_MODULE_REJECTED_BY_EXISTING_POLICY')
        plan = {'release': os.uname().release, 'script_sha256': sha(pathlib.Path(__file__)),
                'original': str(original), 'original_sha256': sha(original), 'original_build_id': original_id,
                'backup': str(backup), 'backup_sha256': sha(backup),
                'candidate': str(candidate), 'candidate_sha256': sha(candidate), 'candidate_build_id': candidate_id,
                'capture_binary': str(args.capture_binary.resolve()), 'capture_binary_sha256': sha(args.capture_binary),
                'device': args.device, 'rollback': str(rollback),
                'sig_enforce': policy_value, 'candidate_signer': candidate_signer,
                'actions': ['stock short capture', 'rmmod uvcvideo', 'insmod candidate', 'candidate short capture',
                            'rmmod uvcvideo', 'modprobe uvcvideo', 'stock rollback short capture'],
                'writes_to_installed_module': False, 'force_flags': False}
        plan_file.write_text(json.dumps(plan, indent=2) + '\n')
        print(json.dumps(plan))
        return 0
    plan = json.loads(plan_file.read_text())
    require(plan['release'] == os.uname().release and plan['script_sha256'] == sha(pathlib.Path(__file__)), 'PREPARED_IDENTITY_CHANGED')
    for name in ['original', 'backup', 'candidate', 'capture_binary']:
        require(sha(pathlib.Path(plan[name])) == plan[name + '_sha256'], 'PREPARED_BYTES_CHANGED:' + name)
    require(loaded_id() == plan['original_build_id'] and not owners(), 'STOCK_MODULE_OR_OWNERS_CHANGED')
    result = {'plan': plan, 'tainted_before': pathlib.Path('/proc/sys/kernel/tainted').read_text().strip(),
              'commands': [], 'captures': {}, 'candidate_loaded': False, 'rollback_restored': False, 'failure': ''}
    log_file = args.out / 'verification.json'
    def save():
        log_file.write_text(json.dumps(result, indent=2) + '\n')
    def module_command(argv):
        r = run(argv)
        result['commands'].append(r)
        save()
        require(r['exit_code'] == 0, 'MODULE_COMMAND_FAILED: ' + r['stderr'])
    def capture(name):
        settle(pathlib.Path(plan['device']))
        argv = [sys.executable, str(root / 'projects/edgevision/tools/run_baseline.py'), '--out',
                str(args.out / (name + '-supervision')), '--timeout', '25', '--', plan['capture_binary'],
                '--device', plan['device'], '--seconds', '5', '--out', str(args.out / name)]
        r = run(argv)
        supervision = json.loads((args.out / (name + '-supervision') / 'run.json').read_text())
        data = json.loads((args.out / name / 'summary.json').read_text())
        result['captures'][name] = {'launcher': r, 'supervision': supervision, 'capture': data, 'loaded_build_id': loaded_id()}
        save()
        require(r['exit_code'] == 0 and supervision['exit_code'] == 0 and supervision['stop_reason'] == 'process_exit'
                and data['consumed'] > 0, 'CAPTURE_BASELINE_FAILED:' + name)
        require(not owners(), 'CAPTURE_DID_NOT_RELEASE_DEVICE')
    changed = False
    try:
        capture('stock-before')
        changed = True
        module_command(['rmmod', 'uvcvideo'])
        require(loaded_id() is None, 'MODULE_UNLOAD_NOT_CONFIRMED')
        module_command(['insmod', plan['candidate']])
        require(loaded_id() == plan['candidate_build_id'], 'ACTUAL_CANDIDATE_MODULE_NOT_CONFIRMED')
        result['candidate_loaded'] = True
        result['tainted_after_candidate'] = pathlib.Path('/proc/sys/kernel/tainted').read_text().strip()
        save()
        capture('candidate')
    except Exception as error:
        result['failure'] = str(error)
    finally:
        try:
            if changed and loaded_id() != plan['original_build_id']:
                require(not owners(), 'ROLLBACK_BLOCKED_BY_VIDEO_OWNER')
                if loaded_id() is not None:
                    module_command(['rmmod', 'uvcvideo'])
                module_command(['modprobe', 'uvcvideo'])
            require(loaded_id() == plan['original_build_id'], 'STOCK_LOADED_BUILD_ID_NOT_RESTORED')
            require(sha(pathlib.Path(plan['original'])) == plan['original_sha256'], 'INSTALLED_MODULE_BYTES_CHANGED')
            result['rollback_restored'] = True
            if changed:
                capture('stock-after-rollback')
        except Exception as error:
            result['failure'] += '; ROLLBACK: ' + str(error)
        result['final_loaded_build_id'] = loaded_id()
        result['tainted_after'] = pathlib.Path('/proc/sys/kernel/tainted').read_text().strip()
        save()
    require(result['candidate_loaded'] and result['rollback_restored'] and not result['failure'], 'VERIFICATION_FAILED: ' + result['failure'])
    print(json.dumps({'candidate_loaded': True, 'rollback_restored': True,
                      'final_loaded_build_id': result['final_loaded_build_id'], 'tainted_after': result['tainted_after'],
                      'verification': str(log_file)}))
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
