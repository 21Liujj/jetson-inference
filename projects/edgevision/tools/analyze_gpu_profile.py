#!/usr/bin/env python3
"""Inspect real Nsight SQLite GPU intervals, rather than CPU enqueue ranges."""
import argparse
import bisect
import collections
import hashlib
import json
import pathlib
import sqlite3


def analyze(path, warmup):
    db = sqlite3.connect(f'file:{path.resolve()}?mode=ro', uri=True)
    rows = db.execute('SELECT k.start,k.end,k.streamId,k.contextId,s.value '
                      'FROM CUPTI_ACTIVITY_KIND_KERNEL k JOIN StringIds s ON s.id=k.demangledName '
                      'ORDER BY k.start').fetchall()
    fused = [row for row in rows if 'rgb_letterbox_kernel' in row[4]]
    if len(fused) <= warmup:
        raise ValueError('NO_MEASURED_FUSED_KERNELS')
    fused = fused[warmup:]
    window = (fused[0][0], max(row[1] for row in rows))
    model = [row for row in rows if 'rgb_letterbox_kernel' not in row[4] and row[1] > window[0]]
    overlapped = total_ns = overlap_ns = 0
    examples = []
    for pre in fused:
        candidates = [row for row in model if row[2] != pre[2] and row[3] == pre[3]
                      and row[0] < pre[1] and row[1] > pre[0]]
        intervals = sorted((max(pre[0], row[0]), min(pre[1], row[1])) for row in candidates)
        end = pre[0]
        intersect = 0
        for start, finish in intervals:
            intersect += max(0, finish - max(start, end))
            end = max(end, finish)
        total_ns += pre[1] - pre[0]
        overlap_ns += intersect
        if intersect:
            overlapped += 1
            if len(examples) < 8:
                other = candidates[0]
                examples.append({'pre_start_ns': pre[0], 'pre_end_ns': pre[1], 'pre_stream': pre[2],
                                 'model_start_ns': other[0], 'model_end_ns': other[1], 'model_stream': other[2],
                                 'cuda_context': pre[3], 'model_kernel': other[4], 'intersection_ns': intersect})
    apis = dict(db.execute('SELECT s.value,count(*) FROM CUPTI_ACTIVITY_KIND_RUNTIME r '
                           'JOIN StringIds s ON s.id=r.nameId WHERE r.start>=? '
                           'GROUP BY s.value', (window[0],)).fetchall())
    nvtx = dict(db.execute('SELECT coalesce(n.text,s.value),count(*) FROM NVTX_EVENTS n '
                           'LEFT JOIN StringIds s ON s.id=n.textId WHERE n.start>=? '
                           'GROUP BY coalesce(n.text,s.value)', (window[0],)).fetchall())
    return {'schema_version': 1, 'sqlite_sha256': hashlib.sha256(path.read_bytes()).hexdigest(),
            'warmup_fused_kernels_excluded': warmup, 'measured_fused_kernels': len(fused),
            'gpu_kernel_intersections': overlapped, 'fused_total_ns': total_ns,
            'fused_intersection_ns': overlap_ns, 'fused_time_overlapped_fraction': overlap_ns / total_ns,
            'examples': examples, 'cuda_runtime_calls_in_window': apis, 'nvtx_ranges_in_window': nvtx,
            'claim': 'actual GPU intervals on different streams; excludes CPU submission overlap',
            'limitations': ['profile adds overhead; use unprofiled matrix for speed',
                            'warmup cutoff counts fused launches, not an NVTX capture-range fence']}


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--sqlite', type=pathlib.Path, required=True)
    parser.add_argument('--warmup', type=int, default=60)
    parser.add_argument('--out', type=pathlib.Path, required=True)
    args = parser.parse_args()
    result = analyze(args.sqlite, args.warmup)
    with args.out.open('x') as stream:
        json.dump(result, stream, indent=2)
        stream.write('\n')
    print(json.dumps({key: result[key] for key in ('measured_fused_kernels', 'gpu_kernel_intersections',
                                                  'fused_time_overlapped_fraction')}))
