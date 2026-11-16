#!/usr/bin/env python3
"""Independent real model JSON-plan/VQA/protocol-negative probe (no tool mocks)."""
import argparse
import csv
import hashlib
import json
import os
import pathlib
import signal
import subprocess
import time
from run_baseline import sample


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    for name in ['binary','llm','visual','plugin','image','out']:parser.add_argument('--'+name,type=pathlib.Path,required=True)
    args=parser.parse_args();args.out.mkdir(parents=True,exist_ok=False)
    fixture=args.out/'fixture.jpg';fixture.write_bytes(args.image.read_bytes())
    requests=[{'request_id':'model-probe-choice-1','max_tokens':1,'messages':[
        {'role':'system','text':'Choose the next robot tool needed to fulfill the user request. Answer with exactly ONE digit from the following registered tool IDs, without punctuation, explanation or whitespace: 0=get_status, 1=start_capture, 2=stop_capture, 3=capture_snapshot, 4=detect_snapshot, 5=describe_snapshot, 6=get_recent_metrics, 7=create_report, 8=finish. No tool has run yet. Choose only one tool. The host validates arguments and constructs the versioned JSON plan from your selected ID; do not generate JSON.'},
        {'role':'user','text':'请查看摄像头当前是否正常运行，把状态告诉我。'}]},
      {'request_id':'model-probe-vqa-1','max_tokens':64,'messages':[
        {'role':'user','text':'Describe only the visible main objects in one brief sentence. Do not infer hidden things.','image_paths':[str(fixture.resolve())]}]},
      {'request_id':'model-probe-invalid-1','max_tokens':0,'messages':[{'role':'user','text':'test invalid request budget'}]}]
    (args.out/'requests.json').write_text(json.dumps(requests,ensure_ascii=False,indent=2)+'\n')
    env=os.environ.copy();env['EDGELLM_PLUGIN_PATH']=str(args.plugin.resolve())
    argv=[str(args.binary.resolve()),str(args.llm.resolve()),str(args.visual.resolve()),str(args.out.resolve())]
    manifest={'argv':argv,'binary_sha256':hashlib.sha256(args.binary.read_bytes()).hexdigest(),
              'image_sha256':hashlib.sha256(fixture.read_bytes()).hexdigest(),'plugin_sha256':hashlib.sha256(args.plugin.read_bytes()).hexdigest()}
    started=time.monotonic();killed=False
    with (args.out/'stdout.log').open('wb') as log,(args.out/'resources.csv').open('w',newline='') as resources:
        child=subprocess.Popen(argv,stdin=subprocess.PIPE,stdout=log,stderr=subprocess.STDOUT,env=env,start_new_session=True)
        child.stdin.write(('\n'.join(json.dumps(r,ensure_ascii=False) for r in requests)+'\n').encode());child.stdin.close()
        writer=csv.writer(resources);writer.writerow(['elapsed_s','rss_kib','fd_count','mem_available_kib','swap_free_kib'])
        while child.poll() is None:
            memory=dict(row.split(':',1) for row in pathlib.Path('/proc/meminfo').read_text().splitlines() if ':' in row)
            writer.writerow([time.monotonic()-started,*sample(child.pid),int(memory['MemAvailable'].split()[0]),int(memory['SwapFree'].split()[0])]);resources.flush()
            if time.monotonic()-started>180:
                os.killpg(child.pid,signal.SIGKILL);killed=True;break
            time.sleep(.25)
        manifest.update(exit_code=child.wait(timeout=10),forced_kill=killed,elapsed_s=time.monotonic()-started)
    (args.out/'run.json').write_text(json.dumps(manifest,indent=2)+'\n')
    output=(args.out/'stdout.log').read_text(errors='replace')
    responses=[json.loads(row[len('EDGEVISION_VLM_RESULT '):]) for row in output.splitlines() if row.startswith('EDGEVISION_VLM_RESULT ')]
    (args.out/'responses.json').write_text(json.dumps(responses,ensure_ascii=False,indent=2)+'\n')
    assert manifest['exit_code']==0 and not killed and len(responses)==3
    assert responses[0]['success'] and responses[1]['success'] and not responses[2]['success']
    # Protocol v2 bounds the model decision to one token. It is the model's tool
    # choice, not a natural-language keyword rule or a prewritten multi-step plan.
    assert responses[0]['text'].strip()=='0'
    plan={'schema_version':1,'selection_protocol':'registered-tool-id-v2','steps':[{'tool':'get_status','args':{},'timeout_ms':10000}]}
    assert responses[2]['error_code']=='VLM_REQUEST_BUDGET'
    print(json.dumps({'pass':True,'elapsed_s':manifest['elapsed_s'],'plan':plan,'vqa':responses[1]['text']},ensure_ascii=False))


if __name__=='__main__':main()
