#!/usr/bin/env python3
"""Actual ROS2/shared-worker snapshot, detector, VLM and Agent smoke acceptance.

No fixture transport, synthetic detections or model responses. Private images and
reports stay in the requested artifact directory. Every owned child is supervised.
"""
import argparse,hashlib,json,os,pathlib,signal,subprocess,sys,threading,time,traceback
sys.path.insert(0,str(pathlib.Path(__file__).resolve().parents[1]/'agent'))
import rclpy
from edgevision_agent.ros_client import RosCoreClient
from edgevision_agent.runner import AgentRunner
from edgevision_agent.schema import AgentError,planning_prompt


def mono():return time.monotonic_ns()
def process(pid):
    try:
        status=dict(line.split(':',1) for line in pathlib.Path(f'/proc/{pid}/status').read_text().splitlines() if ':' in line)
        value={'pid':pid,'rss_kib':int(status.get('VmRSS','0').split()[0]),'hwm_kib':int(status.get('VmHWM','0').split()[0]),
            'fd_count':len(list(pathlib.Path(f'/proc/{pid}/fd').iterdir()))}
        try:
            roll=dict(line.split(':',1) for line in pathlib.Path(f'/proc/{pid}/smaps_rollup').read_text().splitlines() if ':' in line)
            value['pss_kib']=int(roll['Pss'].split()[0])
        except (OSError,ValueError,KeyError):value['pss_kib']=None
        stat=pathlib.Path(f'/proc/{pid}/stat').read_text().rsplit(')',1)[1].split()
        value['utime_jiffies']=int(stat[11]);value['stime_jiffies']=int(stat[12]);return value
    except (OSError,ValueError,IndexError):return {'pid':pid,'unavailable':True}


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    for key in ('node','model','labels','vlm','llm','visual','plugin','out'):parser.add_argument('--'+key,type=pathlib.Path,required=True)
    parser.add_argument('--device',default='/dev/v4l/by-id/usb-Generic_Web_Camera_20250708V1.000-video-index0')
    args=parser.parse_args();out=args.out.resolve();out.mkdir(parents=True,exist_ok=False)
    snapshots=out/'snapshots';snapshots.mkdir();reports=out/'reports'
    params={'device':args.device,'model':str(args.model.resolve()),'labels':str(args.labels.resolve()),
        'snapshot_root':str(snapshots),'vlm_binary':str(args.vlm.resolve()),'vlm_llm_dir':str(args.llm.resolve()),
        'vlm_visual_dir':str(args.visual.resolve()),'vlm_plugin':str(args.plugin.resolve()),
        'image_hz':0.0,'detection_hz':5.0,'auto_start':True}
    argv=[str(args.node.resolve()),'--ros-args']
    for key,value in params.items():argv+=['-p',f'{key}:={str(value).lower() if type(value) is bool else value}']
    manifest={'schema_version':1,'mode':'real-hardware-and-model','argv':argv,
        'inputs':{key:{'path':str(getattr(args,key).resolve()),'sha256':hashlib.sha256(getattr(args,key).read_bytes()).hexdigest()}
            for key in ('node','vlm','model','labels','plugin')},'started_mono_ns':mono(),
        'memory_scope':'separate RSS/PSS samples; RSS is not summed with GPU allocations or called unique physical memory',
        'clock_ticks_per_s':os.sysconf('SC_CLK_TCK')}
    receipts=[];client=None;node=None;sampler=None;stopped=threading.Event();success=False;error=''
    def save(name,value):
        (out/name).write_text(json.dumps(value,ensure_ascii=False,indent=2)+'\n')
    try:
        with (out/'node.log').open('wb') as log:
            node=subprocess.Popen(argv,stdout=log,stderr=subprocess.STDOUT,start_new_session=True)
            manifest['node_pid']=node.pid
        rclpy.init();client=RosCoreClient()
        deadline=mono()+40_000_000_000
        while True:
            status=client.get_status(deadline=deadline)
            if status['capture_state']=='RUNNING' and status['model_state']=='READY':break
            if node.poll() is not None or mono()>=deadline:raise RuntimeError('NODE_NOT_READY')
            time.sleep(.05)
        save('initial-status.json',status)
        def observe():
            count=0
            with (out/'resources.jsonl').open('w') as stream:
                while not stopped.is_set() and count<2400:
                    point={'mono_ns':mono(),'parent':process(node.pid)}
                    try:
                        # This BSP lacks /proc/task/children. Read only PPID
                        # fields, retain only the owned node's direct children.
                        children=set()
                        for f in pathlib.Path('/proc').glob('[0-9]*/stat'):
                            try:
                                fields=f.read_text().rsplit(')',1)[1].split()
                                if int(fields[1])==node.pid:children.add(int(f.parent.name))
                            except (OSError,IndexError,ValueError):pass
                        point['children']=[process(pid) for pid in sorted(children)]
                        memory=dict(line.split(':',1) for line in pathlib.Path('/proc/meminfo').read_text().splitlines() if ':' in line)
                        point['system_mem_available_kib']=int(memory['MemAvailable'].split()[0]);point['swap_free_kib']=int(memory['SwapFree'].split()[0])
                        point['status']=client.get_status(deadline=mono()+2_000_000_000)
                    except Exception as exc:point['sample_error']=str(exc)[:160]
                    stream.write(json.dumps(point,ensure_ascii=False)+'\n');stream.flush();count+=1;stopped.wait(.25)
        sampler=threading.Thread(target=observe);sampler.start()
        cancel=threading.Event()
        def action(op,suffix,seconds=30,**kwargs):
            value=client.action(op,'m7-smoke-'+suffix,mono()+int(seconds*1e9),cancel,**kwargs)
            receipts.append({'operation':op,'result':value});save('receipts.json',receipts);return value
        snapshot=action('snapshot','snapshot');image=pathlib.Path(snapshot['path'])
        assert image.is_relative_to(snapshots) and image.is_file()
        snapshot['image_sha256']=hashlib.sha256(image.read_bytes()).hexdigest()
        detected=action('detect','detect',snapshot_id=snapshot['snapshot_id'])
        assert detected['frame_id']==snapshot['frame_id'] and detected['generation']==snapshot['generation']
        planned=action('plan','plan',prompt=planning_prompt('查看当前相机状态',client.get_status(),[]))
        assert planned['model_response']['success'] and planned['model_response']['generated_tokens']==1
        described=action('describe','describe',snapshot_id=snapshot['snapshot_id'],prompt='Describe only what is visible. Say when the image is too dark or blurry to identify objects.')
        assert described['frame_id']==snapshot['frame_id'] and described['generation']==snapshot['generation'] and described['answer_source']=='local_vlm'
        restored=action('resume_detector','resume');assert restored['detector_ready']
        final=client.get_status();assert final['metrics']['valid_frames']>status['metrics']['valid_frames']
        save('after-switch-status.json',final)
        agent=AgentRunner(client,snapshots,reports)
        trace=agent.run('查看当前相机状态','m7-smoke-agent-status',timeout_s=120)
        save('agent-status-result.json',trace)
        assert trace['mode']=='model-driven' and trace['status']=='DONE' and any(s['tool']=='get_status' for s in trace['steps'])
        success=True
    except Exception as exc:
        error=repr(exc);save('failure.json',{'error':error,'receipt':getattr(exc,'receipt',None),'traceback':traceback.format_exc()[-4096:]})
    finally:
        stopped.set()
        if sampler:sampler.join(timeout=5)
        if client:client.close()
        if rclpy.ok():rclpy.shutdown()
        if node and node.poll() is None:
            os.killpg(node.pid,signal.SIGINT)
            try:node.wait(timeout=25)
            except subprocess.TimeoutExpired:
                success=False;error+='; NODE_SHUTDOWN_TIMEOUT';os.killpg(node.pid,signal.SIGKILL);node.wait(timeout=10)
        manifest.update(normal_exit=success,error=error,node_exit_code=node.returncode if node else None,finished_mono_ns=mono())
        output=(out/'node.log').read_text(errors='replace') if (out/'node.log').exists() else ''
        manifest['physical_shutdown_ack']='EDGEVISION_RUNTIME_STOPPED ' in output
        if not manifest['physical_shutdown_ack'] or node is None or node.returncode!=0:manifest['normal_exit']=False
        save('run.json',manifest)
    print(json.dumps({'pass':manifest['normal_exit'],'error':error,'out':str(out),'physical_shutdown_ack':manifest['physical_shutdown_ack']},ensure_ascii=False))
    raise SystemExit(0 if manifest['normal_exit'] else 1)


if __name__=='__main__':main()
