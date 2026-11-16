#!/usr/bin/env python3
"""Real-device ROS off/diagnostics/5Hz/30Hz publication comparison, no profiler."""
import argparse
import json
import os
import pathlib
import subprocess
import time
import rclpy
from ros2_acceptance import Observer, stop
from run_gpu_matrix import conditions


def resource(pid):
    root=pathlib.Path('/proc')/str(pid)
    fields=(root/'stat').read_text().rsplit(') ',1)[1].split()
    status=dict(row.split(':',1) for row in (root/'status').read_text().splitlines() if ':' in row)
    return {'mono_ns':time.monotonic_ns(),'cpu_ticks':int(fields[11])+int(fields[12]),
            'rss_kib':int(status['VmRSS'].split()[0]),'fd_count':len(list((root/'fd').iterdir()))}


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    for key in ['binary','baseline','model','labels','out']:parser.add_argument('--'+key,type=pathlib.Path,required=True)
    parser.add_argument('--device',required=True)
    args=parser.parse_args();args.out.mkdir(parents=True,exist_ok=False)
    rclpy.init();observer=Observer(args.out);child=None;records=[]
    common=['--device',args.device,'--model',str(args.model.resolve()),'--labels',str(args.labels.resolve()),'--kind','yolov8','--preprocess','cuda']
    try:
        variants=[('ros-off',None),('diagnostics-only',0),('image-5hz',5),('image-30hz',30)]
        for label,hz in variants:
            folder=args.out/label;folder.mkdir()
            if hz is None:
                argv=[str(args.baseline.resolve()),*common,'--seconds','30','--out',str((folder/'capture').resolve())]
            else:
                argv=[str(args.binary.resolve()),'--ros-args','-p','device:='+args.device,
                    '-p','model:='+str(args.model.resolve()),'-p','labels:='+str(args.labels.resolve()),
                    '-p','auto_start:=true','-p','image_hz:='+str(float(hz)),'-p','detection_hz:=30.0',
                    '-p','publish_detection_image:=false','-p','publish_detections:='+('false' if hz==0 else 'true')]
            with (folder/'stdout.log').open('wb') as log:child=subprocess.Popen(argv,stdout=log,stderr=subprocess.STDOUT,start_new_session=True)
            if hz is None:
                observer.until(lambda:(folder/'capture/detections.jsonl').exists() and (folder/'capture/detections.jsonl').stat().st_size>0,45)
            else:
                observer.until(lambda:observer.status.service_is_ready(),20)
                observer.until(lambda:observer.get().capture_state=='RUNNING' and observer.get().model_state=='READY',45)
            observer.spin(6)
            before=observer.get() if hz is not None else None
            first=resource(child.pid);observer_first=resource(os.getpid());samples=[]
            system_before=conditions()
            end=time.monotonic()+20
            while time.monotonic()<end:
                assert child.poll() is None,'MATRIX_CHILD_EARLY_EXIT'
                samples.append(resource(child.pid));observer.spin(.25)
            last=resource(child.pid);observer_last=resource(os.getpid());after=observer.get() if hz is not None else None
            elapsed=(last['mono_ns']-first['mono_ns'])/1e9
            record={'label':label,'argv':argv,'warmup_s':6,'measured_s':elapsed,
                'producer_cpu_fraction_one_core':(last['cpu_ticks']-first['cpu_ticks'])/os.sysconf('SC_CLK_TCK')/elapsed,
                'observer_cpu_fraction_one_core':(observer_last['cpu_ticks']-observer_first['cpu_ticks'])/os.sysconf('SC_CLK_TCK')/elapsed,
                'rss_peak_kib':max(row['rss_kib'] for row in samples),'fd_peak':max(row['fd_count'] for row in samples),
                'latency_scope':'host DQ through completed core detection; DDS delivery excluded',
                'conditions_before':system_before,'conditions_after':conditions()}
            if after:
                a=json.loads(after.metrics_json);b=json.loads(before.metrics_json)
                record.update(before_metrics=b,after_metrics=a)
                record['completed_per_s']=(a['gpu_completions']-b['gpu_completions'])/elapsed
                record['core_p95_ns']=a['dequeue_to_result_p95_ns'];record['core_p99_ns']=a['dequeue_to_result_p99_ns']
                stop(child)
            else:
                child.wait(timeout=15);assert child.returncode==0
                rows=[json.loads(row) for row in (folder/'capture/detections.jsonl').read_text().splitlines()]
                selected=[row for row in rows if first['mono_ns']<=row['completed_mono_ns']<=last['mono_ns']]
                tail=sorted(row['completed_mono_ns']-row['host_dq_ns'] for row in selected if row['completed_mono_ns']>=last['mono_ns']-10_000_000_000)
                assert len(tail)>200
                record['completed_per_s']=len(selected)/elapsed
                record['core_p95_ns']=tail[min(len(tail)-1,int(len(tail)*.95))]
                record['core_p99_ns']=tail[min(len(tail)-1,int(len(tail)*.99))]
            child=None
            (folder/'resource-samples.json').write_text(json.dumps(samples,indent=2)+'\n')
            records.append(record);(args.out/'summary.json').write_text(json.dumps({'simulated':False,'runs':records,
                'limits':'one 20s measured window per configuration, scheduling/system noise retained; not endurance or a precise overhead bound'},indent=2)+'\n')
            print(json.dumps(record),flush=True);observer.spin(1)
    finally:
        if child is not None:stop(child)
        observer.destroy_node();rclpy.shutdown()


if __name__=='__main__':main()
