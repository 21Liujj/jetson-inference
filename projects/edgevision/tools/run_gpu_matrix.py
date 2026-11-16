#!/usr/bin/env python3
"""Bounded, unprofiled P0/P1/P2 comparison; never changes power or clocks."""
import argparse
import datetime
import hashlib
import json
import pathlib
import subprocess
import threading
import time


def digest(path):
    result = hashlib.sha256()
    with path.open('rb') as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b''):
            result.update(chunk)
    return result.hexdigest()


def conditions():
    paths = list(pathlib.Path('/sys/devices/system/cpu').glob('cpu[0-9]*/cpufreq/scaling_*_freq'))
    gpu = pathlib.Path('/sys/devices/platform/17000000.gpu/devfreq/17000000.gpu')
    paths += [gpu / name for name in ('min_freq', 'max_freq', 'cur_freq')]
    paths += list(pathlib.Path('/sys/class/thermal').glob('thermal_zone*/temp'))
    paths += [pathlib.Path('/proc/meminfo'), pathlib.Path('/proc/loadavg')]
    values = {}
    for path in paths:
        try:
            values[str(path)] = path.read_text().strip()
        except (OSError, TypeError) as exc:
            values[str(path)] = {'unavailable': type(exc).__name__}
    return {'wall_utc': datetime.datetime.now(datetime.timezone.utc).isoformat(),
            'mono_ns': time.monotonic_ns(), 'values': values}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', type=pathlib.Path, required=True)
    parser.add_argument('--model', type=pathlib.Path, required=True)
    parser.add_argument('--engine', type=pathlib.Path, required=True)
    parser.add_argument('--corpus', type=pathlib.Path, required=True)
    parser.add_argument('--out', type=pathlib.Path, required=True)
    parser.add_argument('--iterations', type=int, default=1000)
    parser.add_argument('--warmup', type=int, default=60)
    parser.add_argument('--repeats', type=int, default=3)
    args = parser.parse_args()
    if not 1 <= args.repeats <= 5 or not 100 <= args.iterations <= 10000 or not 10 <= args.warmup <= 1000:
        parser.error('INVALID_MATRIX_BUDGET')
    args.out.mkdir(parents=True, exist_ok=False)
    manifest = {'schema_version': 1, 'simulated': False, 'profiler': False,
                'changes_power_or_clocks': False, 'argv': vars(args).copy(),
                'binary_sha256': digest(args.binary), 'engine_sha256': digest(args.engine),
                'model_sha256': digest(args.model), 'corpus_list_sha256': digest(args.corpus / 'inputs.tsv'),
                'scope': 'offline fixed RGB corpus; same display NMS; not camera FPS'}
    manifest['argv'] = {k: str(v) if isinstance(v, pathlib.Path) else v for k, v in manifest['argv'].items()}
    (args.out / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
    stopped = threading.Event()

    def telemetry():
        with (args.out / 'conditions.jsonl').open('w') as stream:
            while not stopped.is_set():
                stream.write(json.dumps(conditions()) + '\n')
                stream.flush()
                stopped.wait(.5)

    observer = threading.Thread(target=telemetry)
    observer.start()
    records = []
    try:
        variants = [('cpu', 'pinned'), ('p1', 'pinned'), ('p1', 'mapped'), ('p2', 'pinned'), ('p2', 'mapped')]
        for repeat in range(args.repeats):
            # Rotate order to expose, rather than hide, warm/thermal order effects.
            order = variants[repeat:] + variants[:repeat]
            for mode, memory in order:
                label = f'r{repeat + 1}-{mode}-{memory}'
                argv = [str(args.binary.resolve()), str(args.model.resolve()), str(args.corpus.resolve()),
                        str((args.out / label).resolve()), mode, memory, str(args.iterations), str(args.warmup)]
                record = {'label': label, 'argv': argv, 'before': conditions()}
                with (args.out / f'{label}.log').open('wb') as log:
                    run = subprocess.run(argv, stdout=log, stderr=subprocess.STDOUT, timeout=180)
                record.update(exit_code=run.returncode, after=conditions())
                records.append(record)
                (args.out / 'runs.json').write_text(json.dumps(records, indent=2) + '\n')
                if run.returncode:
                    raise RuntimeError(f'MATRIX_RUN_FAILED:{label}:{run.returncode}')
                summary = json.loads((args.out / label / 'summary.json').read_text())
                if summary['completed'] != args.iterations or not summary['normal_exit']:
                    raise RuntimeError(f'MATRIX_COUNT_MISMATCH:{label}')
                print(json.dumps({'label': label, **summary}), flush=True)
        (args.out / 'complete.json').write_text(json.dumps({'normal_exit': True, 'runs': len(records)}) + '\n')
    finally:
        stopped.set()
        observer.join(timeout=5)


if __name__ == '__main__':
    main()
