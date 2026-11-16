import hashlib
import json
import re

TOOLS=('get_status','start_capture','stop_capture','capture_snapshot','detect_snapshot',
       'describe_snapshot','get_recent_metrics','create_report','finish')
REQUEST_ID=re.compile(r'^[A-Za-z0-9_.:-]{1,64}$')


class AgentError(RuntimeError):
    def __init__(self,code,receipt=None):
        super().__init__(code)
        self.receipt=receipt


def request_key(request_id):
    if not REQUEST_ID.fullmatch(request_id):raise AgentError('INVALID_REQUEST_ID')
    return hashlib.sha256(request_id.encode()).hexdigest()[:32]


def validate_choice(result, fixture=False):
    if result.get('schema_version')!=1 or result.get('selection_protocol')!='registered-tool-id-v2':
        raise AgentError('PLAN_SCHEMA')
    choice=result.get('tool_id')
    if type(choice) is not int or not 0<=choice<len(TOOLS) or result.get('tool')!=TOOLS[choice]:
        raise AgentError('PLAN_TOOL_NOT_REGISTERED')
    if not fixture:
        proof=result.get('model_response',{})
        if result.get('model')!='Qwen3-VL-2B-Instruct' or not proof.get('success') or proof.get('generated_tokens')!=1:
            raise AgentError('MODEL_PARTICIPATION_NOT_PROVEN')
        if proof.get('text','').strip()!=str(choice):raise AgentError('MODEL_CHOICE_MISMATCH')
    return TOOLS[choice]


def trim_bytes(text, maximum):
    return str(text).encode()[:maximum].decode('utf-8',errors='ignore')


def planning_prompt(goal, status, steps):
    if not goal.strip() or len(goal.encode())>800:raise AgentError('GOAL_BYTE_BUDGET')
    observations=[]
    for step in steps[-3:]:
        result=step.get('result',{})
        observations.append({'tool':step['tool'],'status':step['status'],
            'task_id':step.get('task_id'),'frame_id':step.get('frame_id'),
            'summary':trim_bytes(result.get('summary',step.get('error_code','')),64)})
    done=[s['tool'] for s in steps if s['status']=='DONE']
    text=(f"用户目标 / User goal: {goal}\n相机状态 / Capture: {status['capture_state']}\n"
          f"已经真实完成 / DONE: {done}\n实际结果: "+json.dumps(observations,ensure_ascii=False,separators=(',',':'))+
          '\n选择下一步，已全部完成则选8。 / NEXT tool ID (8 if goal completed):')
    if len(text.encode())>1600:raise AgentError('PLANNING_CONTEXT_BYTE_BUDGET')
    return text


def tool_arguments(tool, context):
    if tool in ('get_status','stop_capture','create_report','finish'):return {}
    if tool=='start_capture':return {'config_id':'usb-yuyv-640x360-30'}
    if tool=='capture_snapshot':return {'freshness_ms':500}
    if tool in ('detect_snapshot','describe_snapshot'):
        if 'snapshot' not in context:raise AgentError('SNAPSHOT_DEPENDENCY_MISSING')
        args={'snapshot_id':context['snapshot']['snapshot_id']}
        if tool=='describe_snapshot':
            if len(context['goal'].encode())>600:raise AgentError('VQA_QUESTION_BYTE_BUDGET')
            args['question']=context['goal']
        else:args['model_id']='yolov8n-coco'
        return args
    if tool=='get_recent_metrics':return {'window_s':10}
    raise AgentError('TOOL_NOT_REGISTERED')
