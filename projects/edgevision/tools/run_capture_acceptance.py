"""Real-device M2 short acceptance. Never opens a second successful stream.

The busy case deliberately attempts a second open/start while a task-owned
probe holds the camera. All images stay in RAM; artifacts are metadata/logs.
"""
import argparse
import csv
import hashlib
import json
import pathlib
import signal
import statistics
import subprocess
import time


def atomic_json(path, data):
    temp = path.with_suffix(path.suffix + '.tmp')
    temp.write_text(json.dumps(data, indent=2) + '\n', encoding='utf-8')
    temp.replace(path)


def describe(values):
    if not values:
        return None
    ordered = sorted(values)
    return {'count': len(values), 'min': ordered[0], 'median': statistics.median(values),
            'p95': ordered[max(0, (95 * len(values) + 99) // 100 - 1)], 'max': ordered[-1]}


def analyze(directory):
    result = json.loads((directory / 'summary.json').read_text())
    with (directory / 'frames.csv').open() as f:
        frames = list(csv.DictReader(f))
    with (directory / 'cycles.csv').open() as f:
        cycles = list(csv.DictReader(f))
    assert len(frames) == result['consumed']
    assert len(cycles) == result['cycles_completed']
    intervals, driver_to_host, queue_age = [], [], []
    previous = None
    for frame in frames:
        queue_age.append(int(frame['consume_ns']) - int(frame['host_dq_ns']))
        if frame['driver_ns'] and frame['driver_clock'] == '1':
            driver_to_host.append(int(frame['host_dq_ns']) - int(frame['driver_ns']))
            if previous and previous['generation'] == frame['generation']:
                intervals.append(int(frame['driver_ns']) - int(previous['driver_ns']))
        previous = frame
    result['metadata_rows'] = len(frames)
    result['driver_interval_ns_at_consumer'] = describe(intervals)
    result['driver_to_host_ns'] = describe(driver_to_host)
    result['driver_to_host_note'] = 'Only MONOTONIC driver timestamps; SOE includes exposure/USB transfer. Not inference latency.'
    result['host_dequeue_to_consume_ns'] = describe(queue_age)
    result['fd_after_stop'] = describe([int(c['fd']) for c in cycles])
    result['rss_kib_after_stop'] = describe([int(c['rss_kib']) for c in cycles])
    result['video_maps_after_stop_max'] = max(int(c['video_maps']) for c in cycles)
    result['stop_ns'] = describe([int(c['stop_ns']) for c in cycles])
    result['queue_high_water_max'] = max(int(c['queue_high_water']) for c in cycles)
    return result


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--binary', required=True, type=pathlib.Path)
    parser.add_argument('--device', required=True)
    parser.add_argument('--out', required=True, type=pathlib.Path)
    args = parser.parse_args()
    binary = args.binary.resolve()
    assert binary.is_file()
    out = args.out.resolve()
    out.mkdir(parents=True, exist_ok=False)
    base = [str(binary), '--device', args.device]
    ledger = {'scope': 'Real camera short capture/control acceptance, no GPU inference or reconnect acceptance',
              'binary_sha256': hashlib.sha256(binary.read_bytes()).hexdigest(),
              'runner_sha256': hashlib.sha256(pathlib.Path(__file__).read_bytes()).hexdigest(), 'runs': [], 'complete': False}
    active = None

    def stop(signum, _):
        if active and active.poll() is None:
            active.send_signal(signal.SIGINT)
        raise KeyboardInterrupt

    signal.signal(signal.SIGINT, stop)
    signal.signal(signal.SIGTERM, stop)

    def run(name, options, expected=0, timeout=30, command=None):
        nonlocal active
        command = command or base + options + ['--out', str(out / name)]
        started = time.monotonic_ns()
        record = {'name': name, 'argv': command, 'expected_exit': expected, 'start_mono_ns': started}
        ledger['runs'].append(record)
        atomic_json(out / 'ledger.json', ledger)
        with (out / (name + '.log')).open('wb') as log:
            active = subprocess.Popen(command, stdout=log, stderr=subprocess.STDOUT)
            record['pid'] = active.pid
            try:
                code = active.wait(timeout=timeout)
            except subprocess.TimeoutExpired:
                active.send_signal(signal.SIGINT)
                try:
                    code = active.wait(timeout=15)
                except subprocess.TimeoutExpired:
                    active.kill()
                    code = active.wait()
                    record['forced_process_cleanup'] = True
                record['timed_out'] = True
            except BaseException:
                if active.poll() is None:
                    active.send_signal(signal.SIGINT)
                    try:
                        active.wait(timeout=15)
                    except subprocess.TimeoutExpired:
                        active.kill()
                        active.wait()
                record['interrupted'] = True
                raise
            finally:
                active = None
        record.update(exit_code=code, elapsed_ns=time.monotonic_ns() - started)
        record['passed'] = code == expected and not record.get('timed_out')
        if code == 0:
            record['analysis'] = analyze(out / name)
        atomic_json(out / 'ledger.json', ledger)
        if not record['passed']:
            raise RuntimeError('Unexpected result: ' + name)
        print(name, 'PASS', 'code=' + str(code), flush=True)

    try:
        run('normal', ['--seconds', '10'])
        run('slow-consumer', ['--seconds', '10', '--consumer-ms', '150'])
        run('pool-exhaustion', ['--seconds', '5', '--hold-slots', '4'])
        run('cycles-100', ['--cycles', '100', '--seconds', '3', '--frames', '3'], timeout=180)
        missing = '/dev/v4l/by-id/edgevision-missing-' + str(time.monotonic_ns())
        run('missing-device', [], expected=1, command=[str(binary), '--device', missing, '--seconds', '1', '--out', str(out / 'missing-device')])
        assert 'OPEN:' in (out / 'missing-device.log').read_text()
        # The owner's CSV buffer becomes nonempty only after actual frames;
        # this avoids testing "busy" before STREAMON/first-frame success.
        owner_dir = out / 'busy-owner'
        owner_command = base + ['--seconds', '6', '--out', str(owner_dir)]
        with (out / 'busy-owner.log').open('wb') as log:
            owner = subprocess.Popen(owner_command, stdout=log, stderr=subprocess.STDOUT)
            try:
                limit = time.monotonic() + 4
                while time.monotonic() < limit:
                    frames = owner_dir / 'frames.csv'
                    if frames.is_file() and frames.stat().st_size > 1000:
                        break
                    if owner.poll() is not None:
                        raise RuntimeError('Busy owner exited early')
                    time.sleep(.02)
                else:
                    raise RuntimeError('Busy owner never delivered a frame')
                run('busy-device', ['--seconds', '1'], expected=1)
                assert 'Device or resource busy' in (out / 'busy-device.log').read_text()
                assert owner.wait(timeout=12) == 0
            finally:
                if owner.poll() is None:
                    owner.send_signal(signal.SIGINT)
                    owner.wait(timeout=15)
        ledger['busy_owner'] = {'argv': owner_command, 'analysis': analyze(owner_dir)}
        ledger['complete'] = True
        atomic_json(out / 'ledger.json', ledger)
        print('PASS all real capture cases; no inference/endurance/reconnect claim', flush=True)
    finally:
        if active and active.poll() is None:
            active.send_signal(signal.SIGINT)
            active.wait(timeout=15)
        atomic_json(out / 'ledger.json', ledger)


if __name__ == '__main__':
    main()
