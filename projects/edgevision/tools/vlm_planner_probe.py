#!/usr/bin/env python3
"""Actual model prompt regression; no tool execution, not Agent acceptance."""
import argparse,json,os,pathlib,signal,subprocess,time

SYSTEM_ZH='你是机器人视觉任务规划器。只回复一个数字，不要解释。工具编号：0=查询设备状态get_status，1=启动采集start_capture，2=停止采集stop_capture，3=拍照capture_snapshot，4=检测照片detect_snapshot，5=描述照片describe_snapshot，6=读取最近指标get_recent_metrics，7=生成报告create_report，8=结束finish。根据用户目标和已经完成的真实操作，选择下一项尚未完成且必要的工具。相机正在运行就不再启动。检测和描述之前需要照片。绝不重复已经完成的工作。用户目标已全部完成时必须回复8。'
SYSTEM_EN='Select the NEXT missing operation for the user goal. Reply with one digit only. 0=get_status, 1=start_capture, 2=stop_capture, 3=capture_snapshot, 4=detect_snapshot, 5=describe_snapshot, 6=get_recent_metrics, 7=create_report, 8=finish. The DONE list contains actual successful tool calls, not proposed actions. Never repeat completed work. If the goal is already satisfied, choose 8. A running camera needs no start. A snapshot is required before detection or description.'
SYSTEM_VERIFY='你是机器人任务的执行结果核验器。根据用户目标和已经真实完成的工具结果，判断是否还缺少用户要求的工作。只回复一个数字：1=用户目标已完成，已有结果足以回答，不需要再执行工具；0=仍缺少用户要求的操作。不要自行增加用户没有要求的任务。查询状态不需要拍照，拍照检测报告需要对应的真实结果。'


def main():
    p=argparse.ArgumentParser(description=__doc__)
    for k in ('binary','llm','visual','plugin','out'):p.add_argument('--'+k,type=pathlib.Path,required=True)
    a=p.parse_args();a.out.mkdir(parents=True,exist_ok=False)
    cases=[('查看当前相机状态',[],0),('查看当前相机状态',['get_status'],8),
        ('拍照，检测并生成报告',[],3),('拍照，检测并生成报告',['capture_snapshot'],4),
        ('拍照，检测并生成报告',['capture_snapshot','detect_snapshot'],7),
        ('拍照，检测并生成报告',['capture_snapshot','detect_snapshot','create_report'],8)]
    requests=[]
    for revision,system in [('zh-short-v3',SYSTEM_ZH),('en-short-v3',SYSTEM_EN),('verify-completion-v1',SYSTEM_VERIFY)]:
        for i,(goal,done,expected) in enumerate(cases):
            prompt=f'用户目标 / User goal: {goal}\n相机状态 / Capture: RUNNING\n已经真实完成 / DONE: {done}\n选择下一步，已全部完成则选8。 / NEXT tool ID (8 if goal completed):'
            if revision=='verify-completion-v1':
                prompt=f'用户目标：{goal}\n已经真实完成的工具：{done}\n该目标已完成吗？只回复1或0。'
                expected=int(expected==8)
            requests.append({'request_id':f'{revision}-{i}','max_tokens':1,'expected':expected,
                'messages':[{'role':'system','text':system},{'role':'user','text':prompt}]})
    (a.out/'requests.json').write_text(json.dumps(requests,ensure_ascii=False,indent=2)+'\n')
    env=os.environ.copy();env['EDGELLM_PLUGIN_PATH']=str(a.plugin.resolve());started=time.monotonic_ns()
    with (a.out/'stdout.log').open('wb') as log:
        child=subprocess.Popen([str(a.binary.resolve()),str(a.llm.resolve()),str(a.visual.resolve()),str(a.out.resolve())],
            stdin=subprocess.PIPE,stdout=log,stderr=subprocess.STDOUT,env=env,start_new_session=True)
        wire=[{k:v for k,v in r.items() if k!='expected'} for r in requests]
        try:child.communicate(('\n'.join(json.dumps(r,ensure_ascii=False) for r in wire)+'\n').encode(),timeout=120)
        except subprocess.TimeoutExpired:os.killpg(child.pid,signal.SIGKILL);child.wait(timeout=10)
    responses=[json.loads(row[22:]) for row in (a.out/'stdout.log').read_text(errors='replace').splitlines() if row.startswith('EDGEVISION_VLM_RESULT ')]
    index={r['request_id']:r for r in responses}
    result={'schema_version':1,'mode':'real-model-prompt-regression-no-tool-execution','exit_code':child.returncode,
        'elapsed_s':(time.monotonic_ns()-started)/1e9,'cases':[]}
    for r in requests:
        actual=index.get(r['request_id'],{})
        result['cases'].append({'id':r['request_id'],'expected_tool_id':r['expected'],'model_response':actual,
            'correct':actual.get('success',False) and actual.get('text','').strip()==str(r['expected'])})
    (a.out/'result.json').write_text(json.dumps(result,ensure_ascii=False,indent=2)+'\n')
    print(json.dumps({'exit_code':child.returncode,'cases':[{k:v for k,v in r.items() if k!='model_response'}|{'actual':r['model_response'].get('text')} for r in result['cases']]},ensure_ascii=False))


if __name__=='__main__':main()
