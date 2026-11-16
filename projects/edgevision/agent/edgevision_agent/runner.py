import datetime
import hashlib
import html
import json
import pathlib
import shutil
import threading
import time
import uuid
from .schema import AgentError,planning_prompt,request_key,tool_arguments,validate_choice
from .metrics import sample_window


class AgentRunner:
    def __init__(self,client,snapshot_root,report_root,allow_fixture=False):
        self.client=client;self.fixture=client.mode!='ros2-real'
        if self.fixture and not allow_fixture:raise AgentError('SIMULATED_BACKEND_FORBIDDEN')
        self.snapshot_root=pathlib.Path(snapshot_root).resolve()
        self.report_root=pathlib.Path(report_root).resolve();self.report_root.mkdir(parents=True,exist_ok=True)
        self.lock=threading.Lock()

    @staticmethod
    def checked_result(value):
        if len(json.dumps(value,ensure_ascii=False,separators=(',',':')).encode())>16384:
            raise AgentError('TOOL_RESULT_BYTE_BUDGET')
        return value

    def checked_image(self,snapshot):
        path=pathlib.Path(snapshot['path']).resolve()
        if not path.is_relative_to(self.snapshot_root) or not path.is_file() or path.stat().st_size>4*1024*1024:
            raise AgentError('SNAPSHOT_PATH_OR_SIZE')
        return path

    def report(self,context,directory):
        evidence=[{'tool':s['tool'],'task_id':s.get('task_id'),'frame_id':s.get('frame_id'),
                   'generation':s.get('generation'),'status':s['status'],'summary':s.get('result',{}).get('summary','')}
                  for s in context['steps']]
        image=''
        if 'snapshot' in context:
            source=self.checked_image(context['snapshot']);shutil.copyfile(source,directory/'snapshot.png')
            image='<img src="snapshot.png" alt="实际快照" style="max-width:100%">'
        document='<!doctype html><html lang="zh-CN"><meta charset="utf-8"><title>EdgeVision 任务报告</title><style>body{font:15px system-ui;max-width:960px;margin:32px auto;padding:0 16px;color:#17212b}pre{white-space:pre-wrap;background:#f3f5f7;padding:16px}img{border:1px solid #d8dee5}</style><h1>EdgeVision 任务报告</h1><p>'+html.escape(context['goal'])+'</p>'+image+'<h2>实际工具证据</h2><pre>'+html.escape(json.dumps(evidence,ensure_ascii=False,indent=2))+'</pre><p>检测框来自固定检测模型；图像描述来自本地 VLM。相机未标定，不推断距离。仅记录已完成工具返回的数据。</p></html>'
        if len(document.encode())>128*1024:raise AgentError('REPORT_BYTE_BUDGET')
        (directory/'report.html').write_text(document,encoding='utf-8')
        return {'report_path':str(directory/'report.html'),'evidence_task_ids':[x['task_id'] for x in evidence if x['task_id'] is not None],
                'summary':f'报告包含 {len(evidence)} 条实际工具记录'}

    def run(self,goal,request_id=None,timeout_s=180,canceled=None):
        canceled=canceled or threading.Event();request_id=request_id or uuid.uuid4().hex;key=request_key(request_id)
        if not goal.strip() or len(goal.encode())>800:raise AgentError('GOAL_BYTE_BUDGET')
        if not 1<=timeout_s<=300:raise AgentError('AGENT_DEADLINE_BUDGET')
        directory=self.report_root/('ag-'+key)
        if directory.exists():
            if directory.is_symlink():raise AgentError('REPORT_PATH_SYMLINK')
            previous=directory/'trace.json'
            if not previous.is_file():raise AgentError('REQUEST_INCOMPLETE_FROM_PRIOR_RUNTIME')
            if previous.stat().st_size>256*1024:raise AgentError('PRIOR_TRACE_BYTE_BUDGET')
            old=json.loads(previous.read_text(encoding='utf-8'))
            if old.get('request_id')!=request_id or old.get('goal')!=goal:raise AgentError('REQUEST_ID_CONFLICT')
            if old.get('status')=='RUNNING':raise AgentError('REQUEST_ALREADY_RUNNING')
            return old
        if not self.lock.acquire(blocking=False):raise AgentError('AGENT_BUSY')
        started=time.monotonic_ns();deadline=started+int(timeout_s*1e9)
        created=False;restore_needed=False
        context={'schema_version':1,'request_id':request_id,'goal':goal,'steps':[],
            'mode':'transport-fixture' if self.fixture else 'model-driven','started_mono_ns':started,'planning_calls':[],
            'wall_utc':datetime.datetime.now(datetime.timezone.utc).isoformat(),'status':'RUNNING'}
        try:
            # Explicit bounded history: preserve at most 64 requested task
            # directories, reject capacity instead of deleting user evidence.
            if len([p for p in self.report_root.iterdir() if p.is_dir() and p.name.startswith('ag-')])>=64:raise AgentError('REPORT_RECORD_BUDGET')
            occupied=sum(p.stat().st_size for folder in self.report_root.iterdir() if folder.is_dir() and not folder.is_symlink() and folder.name.startswith('ag-')
                         for p in folder.iterdir() if p.is_file() and not p.is_symlink())
            if occupied+5*1024*1024>128*1024*1024:raise AgentError('REPORT_STORAGE_BUDGET')
            directory.mkdir(exist_ok=False)
            created=True
            for ordinal in range(7):
                if canceled.is_set():raise AgentError('CANCELED')
                if time.monotonic_ns()>=deadline:raise AgentError('DEADLINE_EXCEEDED')
                status=self.client.get_status(deadline=deadline)
                prompt=planning_prompt(goal,status,context['steps'])
                restore_needed=True
                planning={'ordinal':ordinal,'started_mono_ns':time.monotonic_ns(),'status':'RUNNING'}
                context['planning_calls'].append(planning)
                try:
                    plan=self.checked_result(self.client.action('plan',f'ag-{key}-p{ordinal}',min(deadline,time.monotonic_ns()+30_000_000_000),canceled,prompt=prompt))
                    planning['result']=plan
                    tool=validate_choice(plan,self.fixture)
                    planning['status']='DONE'
                except Exception as error:
                    planning.update(status='FAILED',error_code=str(error)[:256],receipt=getattr(error,'receipt',None));raise
                finally:planning['finished_mono_ns']=time.monotonic_ns()
                if tool=='finish':
                    if not context['steps']:raise AgentError('MODEL_FINISHED_WITHOUT_EVIDENCE')
                    context['final_plan']=plan;context['status']='DONE';break
                if ordinal>=6:raise AgentError('AGENT_STEP_BUDGET')
                args=tool_arguments(tool,context)
                timeout_ms=30000 if tool=='describe_snapshot' else 20000
                planning['validated_plan']={'schema_version':1,'request_id':request_id,
                    'steps':[{'tool':tool,'args':args,'timeout_ms':timeout_ms}],
                    'argument_source':'registered host schema; tool selection from actual model response'}
                step={'tool_call_id':f'{request_id}:s{ordinal}','tool':tool,'args':args,'plan_task_id':plan['task_id'],
                      'timeout_ms':timeout_ms,'started_mono_ns':time.monotonic_ns(),'status':'RUNNING','error_code':''}
                context['steps'].append(step)
                try:
                    tool_deadline=min(deadline,time.monotonic_ns()+20_000_000_000)
                    core_id=f'ag-{key}-t{ordinal}'
                    if tool=='get_status':result=self.client.get_status(deadline=tool_deadline)
                    elif tool=='get_recent_metrics':
                        result=sample_window(self.client,args['window_s'],tool_deadline,canceled)
                    elif tool in ('start_capture','stop_capture'):
                        result=self.client.set_capture(tool=='start_capture',core_id,tool_deadline,canceled)
                    elif tool=='capture_snapshot':
                        result=self.client.action('snapshot',core_id,tool_deadline,canceled,freshness_ms=args['freshness_ms'])
                        path=self.checked_image(result);result['image_sha256']=hashlib.sha256(path.read_bytes()).hexdigest()
                        result['summary']=f"保存快照 frame={result['frame_id']}, generation={result['generation']}"
                        context['snapshot']=result
                    elif tool=='detect_snapshot':
                        result=self.client.action('detect',core_id,tool_deadline,canceled,snapshot_id=args['snapshot_id'])
                        if result['frame_id']!=context['snapshot']['frame_id'] or result['generation']!=context['snapshot']['generation']:
                            raise AgentError('DETECTION_SNAPSHOT_IDENTITY')
                        result['summary']=f"固定检测模型返回 {result['detections_total']} 个目标"
                        counts={}
                        for box in result['detections']:counts[box['label']]=counts.get(box['label'],0)+1
                        result['class_counts']=counts
                        context['detection']=result
                    elif tool=='describe_snapshot':
                        result=self.client.action('describe',core_id,min(deadline,time.monotonic_ns()+30_000_000_000),canceled,
                            snapshot_id=args['snapshot_id'],prompt=args['question'])
                        if result['frame_id']!=context['snapshot']['frame_id'] or result['generation']!=context['snapshot']['generation']:
                            raise AgentError('VQA_SNAPSHOT_IDENTITY')
                        result['summary']=result['answer'];context['description']=result
                    elif tool=='create_report':result=self.report(context,directory)
                    else:raise AgentError('TOOL_NOT_REGISTERED')
                    self.checked_result(result)
                    step.update(status='DONE',result=result,task_id=result.get('task_id'),frame_id=result.get('frame_id'),
                                generation=result.get('generation'),runtime_instance=result.get('runtime_instance'))
                except Exception as error:
                    receipt=getattr(error,'receipt',None) or {}
                    step.update(status='FAILED',error_code=str(error)[:256],error_receipt=receipt,
                        task_id=receipt.get('task_id'),frame_id=receipt.get('frame_id'),generation=receipt.get('generation'));raise
                finally:
                    step['finished_mono_ns']=time.monotonic_ns()
                    text=json.dumps(context,ensure_ascii=False,indent=2)
                    if len(text.encode())>256*1024:raise AgentError('TRACE_BYTE_BUDGET')
                    (directory/'trace.json').write_text(text,encoding='utf-8')
            else:raise AgentError('AGENT_STEP_BUDGET')
        except Exception as error:
            context.update(status='CANCELED' if canceled.is_set() else 'EXPIRED' if time.monotonic_ns()>=deadline else 'FAILED',error_code=str(error)[:256])
        finally:
            if restore_needed:
                try:context['detector_restore']=self.client.restore_detector(f'ag-{key}-restore')
                except Exception as error:
                    context['restore_error']=str(error)[:256]
                    if context['status']=='DONE':context.update(status='FAILED',error_code='DETECTOR_RESTORE_FAILED')
            context['finished_mono_ns']=time.monotonic_ns()
            context['completion_basis']='Actual tool results; model-selected finish. Semantic goal accuracy is evaluated separately.'
            context['answer']=[{'tool':s['tool'],'task_id':s.get('task_id'),'frame_id':s.get('frame_id'),
                               'source':s.get('result',{}).get('answer_source','actual_tool_result'),
                               'text':s.get('result',{}).get('summary',s.get('error_code',''))} for s in context['steps']]
            try:
                if created:
                    text=json.dumps(context,ensure_ascii=False,separators=(',',':'))
                    if len(text.encode())>256*1024:
                        # Keep identities and an explicit failed bounded receipt.
                        # This is never accepted as a complete evidence trace.
                        context={'schema_version':1,'request_id':request_id,'goal':goal,'status':'FAILED',
                            'mode':context['mode'],'error_code':'TRACE_BYTE_BUDGET','incomplete_evidence':True,
                            'oversized_trace_sha256':hashlib.sha256(text.encode()).hexdigest(),
                            'detector_restore':context.get('detector_restore'),'restore_error':context.get('restore_error')}
                        text=json.dumps(context,ensure_ascii=False,separators=(',',':'))
                    (directory/'trace.json').write_text(text,encoding='utf-8')
            finally:self.lock.release()
        return context
