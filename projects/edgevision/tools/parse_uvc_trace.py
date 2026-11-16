#!/usr/bin/env python3
"""Correlate one physical USB path's UVC completion events with consumed-frame CSV.

Kernel stream_id is mapped to (not assumed equal to) userspace generation.
Unconsumed kernel completions are NOT automatically classified as driver loss.
"""
import argparse
import collections
import csv
import json
import math
import pathlib
import re


def parse_events(text,usb):
    frames=[]; streams=[]
    for line in text.splitlines():
        match=re.search(r'\b(uvc_frame|uvc_stream):\s+(.*)',line)
        if not match: continue
        fields=dict(re.findall(r'(\w+)=([^\s]+)',match[2]))
        if fields.get('usb')!=usb: continue
        expected=['interface','video_minor','stream_id'] + (['index','sequence','bytesused','state','error','reason','completed_ns'] if match[1]=='uvc_frame' else ['action','result','mono_ns'])
        if not all(key in fields for key in expected): raise ValueError('INCOMPLETE_TRACE_EVENT')
        event={'usb':usb,**{key:int(fields[key]) for key in expected}}
        (frames if match[1]=='uvc_frame' else streams).append(event)
    return frames,streams


def correlate(frames,rows,max_lag_ns):
    by_sequence=collections.defaultdict(list)
    for event in frames:
        if event['reason']==0: by_sequence[event['sequence']].append(event)
    used=set(); mapping={}; reverse={}; result=[]; unmatched=[]
    for row in rows:
        seq=int(row['sequence']); generation=int(row['generation']); dq=int(row['host_dq_ns'])
        possible=[]
        for event in by_sequence[seq]:
            key=(event['usb'],event['interface'],event['video_minor'],event['stream_id'])
            identity=(*key,seq,event['index'],event['completed_ns'])
            if identity in used or dq<event['completed_ns'] or dq-event['completed_ns']>max_lag_ns: continue
            if generation in mapping and mapping[generation]!=key: continue
            if key in reverse and reverse[key]!=generation: continue
            possible.append((event,key,identity))
        if len(possible)!=1:
            unmatched.append({'generation':generation,'sequence':seq,'candidate_count':len(possible)})
            continue
        event,key,identity=possible[0]; used.add(identity); mapping[generation]=key; reverse[key]=generation
        publish=int(row['publish_ns']); consume=int(row['consume_ns'])
        if publish<dq or consume<publish: raise ValueError('NONMONOTONIC_USER_TIMELINE')
        result.append({'generation':generation,'stream_id':event['stream_id'],'usb':event['usb'],
                       'interface':event['interface'],'video_minor':event['video_minor'],
                       'sequence':seq,'index':event['index'],'bytesused':event['bytesused'],
                       'uvc_error':event['error'],'complete_ns':event['completed_ns'],
                       'host_dq_ns':dq,'publish_ns':publish,'consume_ns':consume,
                       'complete_to_dq_ns':dq-event['completed_ns'],'dq_to_publish_ns':publish-dq,
                       'publish_to_consume_ns':consume-publish})
    return result,unmatched,mapping


def distribution(values):
    if not values: return None
    ordered=sorted(values)
    def percentile(p): return ordered[max(0,math.ceil(len(ordered)*p)-1)]
    return {'n':len(values),'min':ordered[0],'median':percentile(.5),'p95':percentile(.95),'p99':percentile(.99),'max':ordered[-1]}


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--trace',type=pathlib.Path,required=True)
    parser.add_argument('--trace-run',type=pathlib.Path,required=True)
    parser.add_argument('--frames',type=pathlib.Path,required=True)
    parser.add_argument('--usb',required=True)
    parser.add_argument('--out',type=pathlib.Path,required=True)
    parser.add_argument('--max-delivery-lag-ms',type=int,default=1000)
    args=parser.parse_args()
    run=json.loads(args.trace_run.read_text())
    if run.get('failure') or any(run['trace_loss_counters']) or run['trace_clock']!='mono':
        raise ValueError('TRACE_LOSS_OR_CLOCK_UNVERIFIED')
    with args.frames.open(newline='') as f: rows=list(csv.DictReader(f))
    frames,streams=parse_events(args.trace.read_text(),args.usb)
    if not rows or not frames: raise ValueError('NO_REAL_USER_OR_KERNEL_FRAMES')
    timeline,unmatched,mapping=correlate(frames,rows,args.max_delivery_lag_ms*1000000)
    args.out.mkdir(parents=True,exist_ok=False)
    if timeline:
        with (args.out/'timeline.csv').open('w',newline='') as f:
            writer=csv.DictWriter(f,fieldnames=list(timeline[0])); writer.writeheader(); writer.writerows(timeline)
    summary={'scope':'UVC final-copy completion to consumed-frame DQ/publish/consume, all CLOCK_MONOTONIC',
             'usb':args.usb,'kernel_frame_events':len(frames),'delivered_to_vb2':sum(e['reason']==0 for e in frames),
             'internal_corrupt_recycles':sum(e['reason']==1 for e in frames),'cancel_returns':sum(e['reason']==2 for e in frames),
             'delivered_error_buffers':sum(e['reason']==0 and e['error']!=0 for e in frames),
             'user_consumed_rows':len(rows),'matched':len(timeline),'unmatched':unmatched,
             'generation_mapping':{str(g):{'usb':k[0],'interface':k[1],'video_minor':k[2],'kernel_stream_id':k[3]} for g,k in mapping.items()},
             'stream_events':streams,
             'latency_ns':{key:distribution([r[key] for r in timeline]) for key in ('complete_to_dq_ns','dq_to_publish_ns','publish_to_consume_ns')},
             'kernel_completions_not_in_consumed_csv':sum(e['reason']==0 for e in frames)-len(timeline),
             'unconsumed_interpretation':'may be startup/stop, pool drop or READY replacement; correlate counters before assigning a loss cause',
             'pass':not unmatched and len(timeline)==len(rows)}
    (args.out/'summary.json').write_text(json.dumps(summary,indent=2)+'\n')
    print(json.dumps({k:v for k,v in summary.items() if k not in ('unmatched','stream_events')},indent=2))
    if not summary['pass']: raise SystemExit(1)


if __name__=='__main__': main()
