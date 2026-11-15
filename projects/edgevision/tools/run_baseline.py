#!/usr/bin/env python3
"""Run an unmodified upstream executable with bounded supervision and raw logs.

Exit code alone never establishes acceptance. Inspect actual frame/inference output.
"""
import argparse
import csv
import datetime as dt
import json
import os
import pathlib
import signal
import subprocess
import time


def sample(pid):
    root = pathlib.Path('/proc') / str(pid)
    try:
        status = dict(line.split(':', 1) for line in (root / 'status').read_text().splitlines() if ':' in line)
        return int(status['VmRSS'].split()[0]), len(list((root / 'fd').iterdir()))
    except (OSError, KeyError, ValueError):
        return None, None


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--out', type=pathlib.Path, required=True)
    parser.add_argument('--seconds', type=float, default=0, help='SIGINT after this window; zero waits for normal exit')
    parser.add_argument('--timeout', type=float, default=240)
    parser.add_argument('command', nargs=argparse.REMAINDER)
    args = parser.parse_args()
    argv = args.command[1:] if args.command[:1] == ['--'] else args.command
    if not argv or args.seconds < 0 or args.timeout <= 0 or (args.seconds and args.timeout <= args.seconds):
        parser.error('Provide command and timeout greater than positive duration')
    args.out.mkdir(parents=True, exist_ok=False)
    result = {'schema_version': 1, 'simulated': False, 'argv': argv, 'cwd': os.getcwd(),
              'wall_time': dt.datetime.now(dt.timezone.utc).isoformat(), 'duration_requested_s': args.seconds,
              'timeout_s': args.timeout, 'acceptance': 'requires_log_review'}
    start = time.monotonic()
    stopped = None
    with (args.out / 'stdout.log').open('wb') as output, (args.out / 'resources.csv').open('w', newline='') as resources:
        writer = csv.writer(resources)
        writer.writerow(['elapsed_s', 'rss_kib', 'fd_count'])
        try:
            process = subprocess.Popen(argv, stdout=output, stderr=subprocess.STDOUT, start_new_session=True)
            result['pid'] = process.pid
            while process.poll() is None:
                elapsed = time.monotonic() - start
                writer.writerow([round(elapsed, 6), *sample(process.pid)])
                resources.flush()
                if args.seconds and elapsed >= args.seconds and stopped is None:
                    os.killpg(process.pid, signal.SIGINT)
                    stopped = time.monotonic()
                    result['stop_reason'] = 'requested_duration_sigint'
                if elapsed >= args.timeout or (stopped is not None and time.monotonic() - stopped >= 15):
                    os.killpg(process.pid, signal.SIGKILL)
                    result['stop_reason'] = 'forced_kill_timeout'
                    break
                time.sleep(.5)
            result['exit_code'] = process.wait(timeout=5)
            result.setdefault('stop_reason', 'process_exit')
        except OSError as exc:
            result.update(exit_code=None, stop_reason='launch_failed', error=str(exc))
    result['elapsed_s'] = time.monotonic() - start
    (args.out / 'run.json').write_text(json.dumps(result, indent=2) + '\n', encoding='utf-8')
    print(json.dumps(result))
    return 0 if result['exit_code'] == 0 and result['stop_reason'] != 'forced_kill_timeout' else 1


if __name__ == '__main__':
    raise SystemExit(main())
