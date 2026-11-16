#!/usr/bin/env python3
"""Prepare and execute M4 module comparisons/recovery with an unconditional stock rollback.

No installed module overwrite, depmod, boot change, forced load or signing change.
"""
import argparse
import json
import os
import pathlib
import shutil
import subprocess
import sys
from verify_uvc_baseline import sha, run, build_id, loaded_id, owners, require


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--phase',choices=['prepare','matrix','recovery','restore'],required=True)
    parser.add_argument('--out',type=pathlib.Path,required=True)
    parser.add_argument('--build-record',type=pathlib.Path)
    parser.add_argument('--device',default='/dev/v4l/by-id/usb-Generic_Web_Camera_20250708V1.000-video-index0')
    parser.add_argument('--usb',default='1-2.1')
    args=parser.parse_args()
    require(os.geteuid()==0,'ROOT_REQUIRED')
    root=pathlib.Path(__file__).resolve().parents[3]; project=root/'projects/edgevision'; tools=project/'tools'
    args.out=args.out.resolve(); args.out.relative_to(project/'artifacts')
    plan_file=args.out/'prepared-plan.json'
    if args.phase=='prepare':
        require(args.build_record,'BUILD_RECORD_REQUIRED')
        record=json.loads(args.build_record.read_text())
        require(record['build_exit_code']==0 and record['headers_unchanged'] and not record['imported_crc']['mismatches'],'UNVERIFIED_MODULE_BUILD')
        original=pathlib.Path(subprocess.check_output(['modinfo','-n','uvcvideo'],text=True).strip())
        candidate=args.build_record.resolve().parent/'uvcvideo.ko'
        require(sha(candidate)==record['built_module_sha256'],'CANDIDATE_CHANGED')
        require(loaded_id()==build_id(original) and not owners(),'STOCK_IDENTITY_OR_OWNERS_INVALID')
        require(subprocess.check_output(['modinfo','-F','vermagic',str(original)],text=True).strip()==record['vermagic'],'VERMAGIC_MISMATCH')
        policy=pathlib.Path('/sys/module/module/parameters/sig_enforce').read_text().strip()
        signer=subprocess.check_output(['modinfo','-F','signer',str(candidate)],text=True).strip()
        require(policy!='Y' or bool(signer),'SIGNATURE_POLICY_REJECTS_CANDIDATE')
        args.out.mkdir(parents=True,exist_ok=False)
        backup=args.out/'stock-uvcvideo.ko'; shutil.copyfile(original,backup)
        files={'script':pathlib.Path(__file__),'original':original,'backup':backup,'candidate':candidate,
               'capture_binary':project/'build/inference-native/edgevision_capture_probe',
               'recovery_binary':project/'build/inference-native/edgevision_recovery_probe',
               'trace_tool':tools/'uvc_trace_capture.py','parser':tools/'parse_uvc_trace.py',
               'supervisor':tools/'run_baseline.py','identity_helper':tools/'verify_uvc_baseline.py'}
        plan={'release':os.uname().release,'original_build_id':build_id(original),'candidate_build_id':build_id(candidate),
              'files':{k:{'path':str(p.resolve()),'sha256':sha(p)} for k,p in files.items()},
              'device':args.device,'usb':args.usb,'sig_enforce':policy,'candidate_signer':signer,
              'matrix':'stock/off/on: 3x10s normal + 8s slow consumer(100ms) + 3x2s start/stop',
              'recovery':'separate actual operator unplug/replug, at most 600s; restore in finally',
              'installed_module_overwrite':False,'force_load':False}
        rollback=args.out/'rollback.sh'
        rollback.write_text('#!/bin/sh\nset -eu\nif [ -d /sys/module/uvcvideo ]; then rmmod uvcvideo; fi\nmodprobe uvcvideo\n'); rollback.chmod(0o755)
        plan['rollback_script']={'path':str(rollback),'sha256':sha(rollback)}
        plan_file.write_text(json.dumps(plan,indent=2)+'\n'); print(json.dumps(plan)); return
    plan=json.loads(plan_file.read_text())
    require(plan['release']==os.uname().release,'TARGET_CHANGED')
    for item in plan['files'].values(): require(sha(pathlib.Path(item['path']))==item['sha256'],'PREPARED_BYTES_CHANGED:'+item['path'])
    require(not owners(),'VIDEO_DEVICE_BUSY')
    if args.phase!='restore': require(loaded_id()==plan['original_build_id'],'STOCK_MODULE_REQUIRED_AT_START')
    log_file=args.out/(args.phase+'-verification.json')
    require(not log_file.exists(),'RESULT_ALREADY_EXISTS')
    result={'phase':args.phase,'plan_sha256':sha(plan_file),'commands':[],'runs':[],
            'tainted_before':pathlib.Path('/proc/sys/kernel/tainted').read_text().strip(),'rollback_restored':False,'failure':''}
    def save(): log_file.write_text(json.dumps(result,indent=2)+'\n')
    def checked(argv):
        r=run(argv); result['commands'].append(r); save()
        require(r['exit_code']==0,'COMMAND_FAILED:'+r['stderr']); return r
    def switch_candidate():
        checked(['rmmod','uvcvideo']); checked(['insmod',plan['files']['candidate']['path']])
        require(loaded_id()==plan['candidate_build_id'],'ACTUAL_CANDIDATE_ID_MISMATCH')
        result['candidate_loaded']=True; save()
    def capture(name,mode,extra=None,recovery=False):
        folder=args.out/name; folder.mkdir()
        binary=plan['files']['recovery_binary' if recovery else 'capture_binary']['path']
        probe=[binary,'--device',plan['device'],'--out',str(folder/'capture'),*(extra or [])]
        argv=[sys.executable,str(tools/'uvc_trace_capture.py'),'--mode',mode,'--out',str(folder/'trace'),
              '--buffer-kb','4096' if recovery else '512','--timeout','630' if recovery else '45','--',*probe]
        entry={'name':name,'loaded_build_id':loaded_id(),'argv':argv}; result['runs'].append(entry); save()
        with (folder/'command.log').open('w') as log:
            proc=subprocess.run(argv,stdout=log,stderr=subprocess.STDOUT)
        entry['exit_code']=proc.returncode; save()
        require(proc.returncode==0,'TRACE_CAPTURE_FAILED:'+name)
        if mode=='uvc':
            checked([sys.executable,str(tools/'parse_uvc_trace.py'),'--trace',str(folder/'trace/trace.txt'),
                     '--trace-run',str(folder/'trace/trace-run.json'),'--frames',str(folder/'capture/frames.csv'),
                     '--usb',plan['usb'],'--out',str(folder/'parsed')])
            entry['parsed']=json.loads((folder/'parsed/summary.json').read_text())
        require(not owners(),'CAPTURE_OWNER_LEAK'); save()
    try:
        if args.phase=='matrix':
            for variant in ('stock','disabled','enabled'):
                if variant=='disabled': switch_candidate()
                mode='uvc' if variant=='enabled' else 'off'
                for repeat in range(3): capture(f'{variant}-normal-{repeat}',mode,['--seconds','10'])
                capture(variant+'-slow',mode,['--seconds','8','--consumer-ms','100'])
                capture(variant+'-cycles',mode,['--seconds','2','--cycles','3'])
        elif args.phase=='recovery':
            switch_candidate(); capture('recovery','uvc',['--seconds','600'],True)
    except BaseException as e:
        result['failure']=str(e)
    finally:
        try:
            require(not owners(),'ROLLBACK_BLOCKED_BY_VIDEO_OWNER')
            if loaded_id()!=plan['original_build_id']:
                if loaded_id() is not None: checked(['rmmod','uvcvideo'])
                checked(['modprobe','uvcvideo'])
            require(loaded_id()==plan['original_build_id'],'ACTUAL_STOCK_MODULE_NOT_RESTORED')
            require(sha(pathlib.Path(plan['files']['original']['path']))==plan['files']['original']['sha256'],'STOCK_FILE_CHANGED')
            result['rollback_restored']=True
            # A real capture is required after every experimental phase rollback.
            capture(args.phase+'-stock-after-rollback','off',['--seconds','5'])
        except BaseException as e: result['failure']+='; ROLLBACK: '+str(e)
        result['final_loaded_build_id']=loaded_id()
        result['tainted_after']=pathlib.Path('/proc/sys/kernel/tainted').read_text().strip(); save()
    print(json.dumps({'phase':args.phase,'runs':len(result['runs']),'rollback_restored':result['rollback_restored'],'failure':result['failure']}))
    require(not result['failure'] and result['rollback_restored'],'M4_VERIFICATION_FAILED')


if __name__=='__main__': main()
