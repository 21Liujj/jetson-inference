#!/usr/bin/env python3
"""Summarize actual v4l2-ctl --verbose DQBUF lines (one stream generation)."""
import argparse
import json
import pathlib
import re


def percentile(values, probability):
    if not values:
        return None
    ordered = sorted(values)
    index = (len(ordered) - 1) * probability
    low = int(index)
    high = min(low + 1, len(ordered) - 1)
    return ordered[low] + (ordered[high] - ordered[low]) * (index - low)


def summarize(raw):
    pattern = r'cap dqbuf:\s*\d+\s+seq:\s*(\d+)\s+bytesused:\s*(\d+)\s+ts:\s*([\d.]+)'
    frames = [(int(seq), int(size), float(stamp)) for seq, size, stamp in re.findall(pattern, raw)]
    if len(frames) < 2:
        raise ValueError('Insufficient real DQBUF samples')
    gaps = duplicates = backward = 0
    intervals = []
    for previous, current in zip(frames, frames[1:]):
        delta = (current[0] - previous[0]) & 0xffffffff
        if delta == 0:
            duplicates += 1
        elif delta >= 0x80000000:
            backward += 1
        else:
            gaps += delta - 1
        intervals.append((current[2] - previous[2]) * 1000)
    duration = frames[-1][2] - frames[0][2]
    return {'schema_version': 1, 'simulated': False, 'frames': len(frames),
            'first_sequence': frames[0][0], 'last_sequence': frames[-1][0],
            'driver_timestamp_span_s': duration,
            'driver_interval_fps': (len(frames) - 1) / duration if duration > 0 else None,
            'sequence_gaps': gaps, 'sequence_duplicates': duplicates, 'sequence_backward': backward,
            'bytesused_min': min(f[1] for f in frames), 'bytesused_max': max(f[1] for f in frames),
            'interval_ms': {name: percentile(intervals, p) for name, p in [('p50', .5), ('p95', .95), ('p99', .99), ('max', 1)]},
            'timestamp_type_monotonic': 'ts-monotonic' in raw,
            'timestamp_source_soe': 'ts-src-soe' in raw,
            'limitations': ['Driver timestamp intervals only; no user dequeue timestamps or inference latency.',
                            'One tool run, no warmup exclusion or repeated formal benchmark.',
                            'Sequence gaps cannot identify the physical cause of missing frames.']}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('log', type=pathlib.Path)
    parser.add_argument('--out', type=pathlib.Path, required=True)
    args = parser.parse_args()
    result = summarize(args.log.read_text(encoding='utf-8', errors='replace'))
    args.out.write_text(json.dumps(result, indent=2) + '\n', encoding='utf-8')
    print(json.dumps(result))


if __name__ == '__main__':
    main()
