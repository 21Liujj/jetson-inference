"""Host transport fixtures: these tests do not prove model or ROS participation."""
import json
import pathlib
import sys
import tempfile
import threading
import time
import unittest
from unittest import mock

sys.path.insert(0,str(pathlib.Path(__file__).resolve().parents[1]/'agent'))
from edgevision_agent.schema import AgentError,TOOLS,planning_prompt,validate_choice
from edgevision_agent.runner import AgentRunner
from edgevision_agent.queue import AgentQueue
from edgevision_agent.metrics import sample_window


class TransportFixture:
    mode='fixture'
    def __init__(self,root,choices):
        self.choices=iter(choices);self.calls=[];self.restores=0;self.next_id=100
        self.image=root/'fixture.png';self.image.write_bytes(b'fixture-codec-not-a-camera-image')
    def get_status(self,**_):
        return {'runtime_instance':'fixture-runtime','capture_state':'RUNNING','generation':7,
            'queue_depth':0,'pool_busy':1,'metrics':{},'summary':'fixture status'}
    def action(self,operation,request_id,deadline,canceled,**args):
        self.calls.append(operation);self.next_id+=1
        result={'schema_version':1,'task_id':self.next_id,'generation':7,'runtime_instance':'fixture-runtime'}
        if operation=='plan':
            choice=next(self.choices)
            return dict(result,selection_protocol='registered-tool-id-v2',tool_id=choice,
                tool=TOOLS[choice] if 0<=choice<len(TOOLS) else 'not_registered')
        if operation=='snapshot':return dict(result,snapshot_id='fixture-snapshot',frame_id=42,path=str(self.image))
        if operation=='detect':return dict(result,frame_id=42,detections_total=0,detections=[])
        if operation=='describe':return dict(result,frame_id=42,answer='fixture description',answer_source='fixture')
        raise AssertionError(operation)
    def restore_detector(self,request_id):
        self.restores+=1;return {'detector_ready':True,'fixture':True}


class AgentContracts(unittest.TestCase):
    def setUp(self):
        self.temp=tempfile.TemporaryDirectory();self.root=pathlib.Path(self.temp.name)
        self.snap=self.root/'snap';self.snap.mkdir();self.reports=self.root/'reports'
    def tearDown(self):self.temp.cleanup()
    def runner(self,choices):
        client=TransportFixture(self.snap,choices)
        return client,AgentRunner(client,self.snap,self.reports,allow_fixture=True)
    def test_fixture_cannot_be_presented_as_real_model(self):
        with self.assertRaisesRegex(AgentError,'SIMULATED_BACKEND_FORBIDDEN'):
            AgentRunner(TransportFixture(self.snap,[0]),self.snap,self.reports)
        raw={'schema_version':1,'selection_protocol':'registered-tool-id-v2','tool_id':0,'tool':'get_status'}
        with self.assertRaisesRegex(AgentError,'MODEL_PARTICIPATION_NOT_PROVEN'):validate_choice(raw)
        raw.update(model='Qwen3-VL-2B-Instruct',model_response={'success':True,'generated_tokens':1,'text':'1'})
        with self.assertRaisesRegex(AgentError,'MODEL_CHOICE_MISMATCH'):validate_choice(raw)
    def test_invalid_choice_executes_no_tool_and_restores(self):
        client,runner=self.runner([99]);result=runner.run('查看状态','invalid-tool')
        self.assertEqual(result['status'],'FAILED');self.assertEqual(client.calls,['plan'])
        self.assertEqual(client.restores,1);self.assertEqual(result['planning_calls'][0]['status'],'FAILED')
    def test_request_replay_has_no_second_tool_execution(self):
        client,runner=self.runner([0,8]);first=runner.run('查看状态','same-request')
        calls=list(client.calls);second=runner.run('查看状态','same-request')
        self.assertEqual(first,second);self.assertEqual(client.calls,calls)
        self.assertEqual(first['mode'],'transport-fixture')
        with self.assertRaisesRegex(AgentError,'REQUEST_ID_CONFLICT'):runner.run('其他任务','same-request')
    def test_report_preserves_frame_source_and_escapes_goal(self):
        client,runner=self.runner([3,4,5,7,8]);result=runner.run('<script>alert(1)</script> 描述并检测','report')
        self.assertEqual(result['status'],'DONE');self.assertEqual(len(result['steps']),4)
        report=next(self.reports.glob('ag-*/report.html')).read_text(encoding='utf-8')
        self.assertNotIn('<script>',report);self.assertIn('&lt;script&gt;',report)
        self.assertEqual(result['steps'][2]['result']['answer_source'],'fixture')
        self.assertEqual(result['steps'][1]['frame_id'],42)
        self.assertEqual(result['planning_calls'][0]['validated_plan']['steps'][0]['timeout_ms'],20000)
    def test_failure_receipt_and_restore_are_retained(self):
        client,runner=self.runner([3])
        original=client.action
        def action(operation,*args,**kwargs):
            if operation=='snapshot':raise AgentError('CAPTURE_NOT_RUNNING',{'task_id':77,'generation':7,'execution_pending':False})
            return original(operation,*args,**kwargs)
        client.action=action;result=runner.run('拍照','failed-snapshot')
        self.assertEqual(result['status'],'FAILED');self.assertEqual(result['steps'][0]['task_id'],77)
        self.assertFalse(result['steps'][0]['error_receipt']['execution_pending']);self.assertEqual(client.restores,1)
    def test_invalid_snapshot_dependency_never_calls_detector(self):
        client,runner=self.runner([4]);result=runner.run('检测','missing-snapshot')
        self.assertEqual(result['error_code'],'SNAPSHOT_DEPENDENCY_MISSING');self.assertEqual(client.calls,['plan'])
    def test_oversized_plan_is_rejected_before_tool_execution(self):
        client,runner=self.runner([0]);original=client.action
        def oversized(*args,**kwargs):return dict(original(*args,**kwargs),unbounded='x'*20000)
        client.action=oversized;result=runner.run('查看状态','oversized-plan')
        self.assertEqual(result['status'],'FAILED');self.assertEqual(result['error_code'],'TOOL_RESULT_BYTE_BUDGET')
        self.assertEqual(client.calls,['plan']);self.assertEqual(client.restores,1)
    def test_utf8_input_budget_and_snapshot_path(self):
        status={'capture_state':'RUNNING','generation':7}
        prompt=planning_prompt('物'*260,status,[]);self.assertIn('物'*260,prompt);self.assertIn('DONE: []',prompt)
        with self.assertRaisesRegex(AgentError,'GOAL_BYTE_BUDGET'):planning_prompt('物'*267,status,[])
        _,runner=self.runner([0])
        external=self.root/'outside.png';external.write_bytes(b'not owned')
        with self.assertRaisesRegex(AgentError,'SNAPSHOT_PATH_OR_SIZE'):runner.checked_image({'path':str(external)})
    def test_queue_capacity_idempotency_cancel_and_waiting_deadline(self):
        class BlockingRunner:
            def __init__(self):self.entered=threading.Event();self.release=threading.Event();self.calls=[]
            def run(self,goal,request_id,timeout,canceled):
                self.calls.append(request_id);self.entered.set();self.release.wait(3)
                return {'status':'CANCELED' if canceled.is_set() else 'DONE'}
        runner=BlockingRunner();jobs=AgentQueue(runner)
        try:
            jobs.submit('first','active');self.assertTrue(runner.entered.wait(1))
            pending,_=jobs.submit('second','waiting',1)
            self.assertTrue(jobs.submit('second','waiting',1)[1])
            with self.assertRaisesRegex(AgentError,'REQUEST_ID_CONFLICT'):jobs.submit('second','waiting',2)
            with self.assertRaisesRegex(AgentError,'AGENT_QUEUE_FULL'):jobs.submit('third','overflow')
            time.sleep(1.05);runner.release.set();jobs.pending.join()
            self.assertEqual(jobs.query('waiting')['state'],'EXPIRED');self.assertEqual(runner.calls,['active'])
            runner.release.clear();runner.entered.clear();jobs.submit('again','next-active');self.assertTrue(runner.entered.wait(1))
            jobs.submit('cancel me','pending-cancel');self.assertTrue(jobs.cancel('pending-cancel'))
            runner.release.set();jobs.pending.join();self.assertEqual(jobs.query('pending-cancel')['state'],'CANCELED')
            self.assertNotIn('pending-cancel',runner.calls)
        finally:runner.release.set();jobs.close()
    def test_metrics_uses_actual_interval_and_counter_deltas(self):
        class Clock:
            now=1_000_000_000
            def mono(self):return self.now
            def wait(self,seconds):self.now+=int(seconds*1e9);return False
        clock=Clock()
        class MetricsClient:
            count=0
            def get_status(self,**_):
                self.count+=1
                return {'runtime_instance':'fixture','generation':7,'queue_depth':self.count%3,'pool_busy':2,
                    'metrics':{'valid_frames':100+self.count,'sequence_gaps':9}}
        with mock.patch('edgevision_agent.metrics.time.monotonic_ns',clock.mono):
            result=sample_window(MetricsClient(),10,20_000_000_000,clock)
        self.assertEqual(result['counter_deltas']['sequence_gaps'],0)
        self.assertEqual(result['counter_deltas']['valid_frames'],40)
        self.assertEqual(result['observation'],'forward_sampled_window');self.assertEqual(result['samples'],41)


if __name__=='__main__':unittest.main(verbosity=2)
