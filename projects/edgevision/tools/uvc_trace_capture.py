#!/usr/bin/env python3
"""Capture a bounded trace in an OWN tracefs instance; never reset global tracing."""
import argparse
import json
import os
import pathlib
import re
import resource
import subprocess
import sys
import time


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--mode',choices=['existing','uvc','off'],required=True)
    parser.add_argument('--out',type=pathlib.Path,required=True)
    parser.add_argument('--timeout',type=int,default=30)
    parser.add_argument('--buffer-kb',type=int,default=512)
    parser.add_argument('command',nargs=argparse.REMAINDER)
    args=parser.parse_args()
    if os.geteuid()!=0: parser.error('root required for the owned tracefs instance')
    root=pathlib.Path(__file__).resolve().parents[3]
    args.out=args.out.resolve(); args.out.relative_to(root/'projects/edgevision/artifacts')
    if not args.command or args.command[0]!='--' or args.timeout<1 or args.timeout>900 or args.buffer_kb<64 or args.buffer_kb>8192:
        parser.error('bounded timeout and command after -- required')
    args.out.mkdir(parents=True,exist_ok=False)
    tracing=pathlib.Path('/sys/kernel/tracing')
    if not (tracing/'instances').is_dir(): raise RuntimeError('TRACEFS_INSTANCES_UNAVAILABLE')
    instance=tracing/'instances'/f'edgevision_{os.getpid()}_{time.monotonic_ns()}'
    instance.mkdir()
    events=[]
    record={'mode':args.mode,'trace_clock':'mono','instance':str(instance),'command':args.command[1:]}
    try:
        (instance/'tracing_on').write_text('0')
        (instance/'trace_clock').write_text('mono')
        (instance/'buffer_size_kb').write_text(str(args.buffer_kb))
        record['buffer_kb_per_cpu']=args.buffer_kb
        (instance/'trace').write_text('')
        if args.mode=='existing':
            names=['v4l2/v4l2_qbuf','v4l2/v4l2_dqbuf','v4l2/vb2_v4l2_buf_done','vb2/vb2_buf_done']
        elif args.mode=='uvc': names=['edgevision_uvc/uvc_frame','edgevision_uvc/uvc_stream']
        else: names=[]
        for name in names:
            event=instance/'events'/name
            if not (event/'enable').exists(): raise RuntimeError('EVENT_UNAVAILABLE:'+name)
            (args.out/(name.replace('/','-')+'-format.txt')).write_text((event/'format').read_text())
            (event/'enable').write_text('1'); events.append(event)
        record['enabled_events']=names
        (instance/'tracing_on').write_text('1')
        supervision=args.out/'supervision'
        command=[sys.executable,str(root/'projects/edgevision/tools/run_baseline.py'),
                 '--out',str(supervision),'--timeout',str(args.timeout),'--',*args.command[1:]]
        record['start_mono_ns']=time.monotonic_ns()
        record['cpu_stat_before']=pathlib.Path('/proc/stat').read_text().splitlines()[:1+os.cpu_count()]
        usage_before=resource.getrusage(resource.RUSAGE_CHILDREN)
        with (args.out/'launcher.log').open('w') as log:
            result=subprocess.run(command,stdout=log,stderr=subprocess.STDOUT)
        record['exit_code']=result.returncode
        record['end_mono_ns']=time.monotonic_ns()
        usage_after=resource.getrusage(resource.RUSAGE_CHILDREN)
        record['child_cpu_user_s']=usage_after.ru_utime-usage_before.ru_utime
        record['child_cpu_system_s']=usage_after.ru_stime-usage_before.ru_stime
        record['cpu_stat_after']=pathlib.Path('/proc/stat').read_text().splitlines()[:1+os.cpu_count()]
        record['clock_ticks_per_second']=os.sysconf('SC_CLK_TCK')
        (instance/'tracing_on').write_text('0')
        (args.out/'trace.txt').write_text((instance/'trace').read_text())
        stats={}
        for cpu in sorted((instance/'per_cpu').glob('cpu*')):
            stats[cpu.name]=(cpu/'stats').read_text()
        record['per_cpu_stats']=stats
        overruns=[]
        for text in stats.values():
            for line in text.splitlines():
                if re.match(r'\s*(overrun|commit overrun|dropped events)\s*:',line):
                    overruns.append(int(line.split(':',1)[1].strip()))
        record['trace_loss_counters']=overruns
        if any(overruns): raise RuntimeError('TRACE_BUFFER_LOSS')
        if result.returncode: raise RuntimeError('CAPTURE_COMMAND_FAILED')
    except BaseException as error:
        record['failure']=str(error)
        raise
    finally:
        (instance/'tracing_on').write_text('0')
        for event in events: (event/'enable').write_text('0')
        record['instance_removed']=False
        try: instance.rmdir(); record['instance_removed']=True
        finally: (args.out/'trace-run.json').write_text(json.dumps(record,indent=2)+'\n')
    print(json.dumps({'out':str(args.out),'exit_code':record['exit_code'],'events':record['enabled_events']}))


if __name__=='__main__': main()
