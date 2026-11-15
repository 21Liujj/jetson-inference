#!/usr/bin/env python3
"""Verify and package a completed supervised camera baseline, never a live run."""
import argparse
import csv
import hashlib
import json
import pathlib
import re
import statistics
import zipfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--run', type=pathlib.Path, required=True)
    parser.add_argument('--out', type=pathlib.Path, required=True)
    args = parser.parse_args()
    run = json.loads((args.run / 'run.json').read_text())
    log = (args.run / 'stdout.log').read_text(errors='replace')
    text = re.sub(r'\x1b\[[0-9;]*m', '', log)
    with (args.run / 'resources.csv').open(newline='') as stream:
        samples = list(csv.DictReader(stream))
    resource_samples = [(float(s['elapsed_s']), int(s['rss_kib']), int(s['fd_count']))
                        for s in samples if s['rss_kib'] and s['fd_count']]
    requested = run['duration_requested_s']
    if requested < 360:
        raise ValueError('Long-baseline resource windows require a run of at least six minutes')
    count = len(re.findall(r'Timing Report ', text))
    writes = len(re.findall(r'\[image\]\s+saved ', text))
    errors = [line for line in text.splitlines() if re.search(r'failed to|cudaError|CUDA error|Error Code', line, re.I)]
    checks = {'normal_exit': run['exit_code'] == 0,
              'supervised_window_completed': run['stop_reason'] == 'requested_duration_sigint' and run['elapsed_s'] >= requested,
              'effective_work_observed': count > 0 and count == writes,
              'cleanup_logged': 'detectnet:  shutdown complete.' in text and 'pipeline stopped' in text,
              'no_failure_markers': not errors,
              'resource_samples_cover_window': bool(resource_samples) and resource_samples[-1][0] >= requested - 1}
    steady = [s for s in resource_samples if s[0] >= 60]
    result = {'schema_version': 1, 'simulated': False, 'scope': 'supervised repaired-upstream functional baseline',
              'checks': checks, 'passed': all(checks.values()), 'run': run,
              'inference_reports': count, 'jpeg_output_calls': writes,
              'resource_samples': len(resource_samples), 'failure_markers': errors[:50],
              'post_60s_resources': {'rss_kib_min': min(s[1] for s in steady), 'rss_kib_max': max(s[1] for s in steady),
                                    'fd_min': min(s[2] for s in steady), 'fd_max': max(s[2] for s in steady),
                                    'rss_kib_first_5min_median': statistics.median(s[1] for s in steady if s[0] < 360),
                                    'rss_kib_last_5min_median': statistics.median(s[1] for s in steady if s[0] >= requested - 300)},
              'limitations': ['Verbose logging and real JPEG discard encoding included; not a formal latency/FPS benchmark.',
                             'Only process RSS/fd sampled; not total CPU/GPU memory, temperature or power.',
                             'No per-frame V4L2 sequence/timestamp trace for this upstream GStreamer run.',
                             'Short CPU-core sanitizer builds/tests occurred during this functional window.',
                             'No camera unplug, 100-cycle, 2-hour or 12-hour acceptance.']}
    args.out.mkdir(parents=True, exist_ok=False)
    files = []
    with zipfile.ZipFile(args.out / 'raw-long-run.zip', 'w', zipfile.ZIP_DEFLATED, compresslevel=6) as archive:
        for name in ['run.json', 'resources.csv', 'stdout.log']:
            data = (args.run / name).read_bytes()
            archive.writestr(name, data)
            files.append({'path': name, 'bytes': len(data), 'sha256': hashlib.sha256(data).hexdigest()})
    result['files'] = files
    result['archive_sha256'] = hashlib.sha256((args.out / 'raw-long-run.zip').read_bytes()).hexdigest()
    (args.out / 'summary.json').write_text(json.dumps(result, indent=2) + '\n', encoding='utf-8')
    print(json.dumps(result))
    return 0 if result['passed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
