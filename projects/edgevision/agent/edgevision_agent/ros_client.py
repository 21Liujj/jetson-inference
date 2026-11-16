import json
import threading
import time

import rclpy
from rclpy.action import ActionClient
from rclpy.node import Node
from rclpy.executors import SingleThreadedExecutor
from edgevision_interfaces.srv import GetStatus,SetCapture
from edgevision_interfaces.action import RunInspection
from .schema import AgentError


class RosCoreClient:
    mode='ros2-real'
    def __init__(self):
        self.node=Node('edgevision_agent')
        self.status=self.node.create_client(GetStatus,'/edgevision/get_status')
        self.capture=self.node.create_client(SetCapture,'/edgevision/set_capture')
        self.inspection=ActionClient(self.node,RunInspection,'/edgevision/run_inspection')
        self.executor=SingleThreadedExecutor();self.executor.add_node(self.node)
        self.spinner=threading.Thread(target=self.executor.spin,daemon=True);self.spinner.start()
        self.runtime_instance=None
        if not self.status.wait_for_service(timeout_sec=10) or not self.inspection.wait_for_server(timeout_sec=10):
            self.close();raise AgentError('CORE_NOT_AVAILABLE')

    def close(self):
        self.executor.shutdown(timeout_sec=5)
        if self.spinner.is_alive():self.spinner.join(timeout=5)
        self.node.destroy_node()

    @staticmethod
    def await_future(future, deadline, canceled=None):
        event=threading.Event();future.add_done_callback(lambda _:event.set())
        while not event.wait(.02):
            if canceled is not None and canceled.is_set():raise AgentError('CANCELED')
            if time.monotonic_ns()>=deadline:raise AgentError('DEADLINE_EXCEEDED')
        value=future.result()
        if value is None:raise AgentError('CORE_EMPTY_RESPONSE')
        return value

    def get_status(self,task_id=0,deadline=None):
        deadline=deadline or time.monotonic_ns()+5_000_000_000
        value=self.await_future(self.status.call_async(GetStatus.Request(task_id=task_id)),deadline)
        metrics=json.loads(value.metrics_json)
        instance=metrics.get('runtime_instance')
        if not instance:raise AgentError('CORE_RUNTIME_ID_MISSING')
        if self.runtime_instance is None:self.runtime_instance=instance
        if instance!=self.runtime_instance:raise AgentError('CORE_RUNTIME_RESTARTED')
        return {'schema_version':value.schema_version,'runtime_instance':instance,
            'capture_state':value.capture_state,'model_state':value.model_state,'generation':value.generation,
            'resources_live':value.resources_live,'queue_depth':value.queue_depth,'pool_busy':value.pool_busy,
            'capture_error':value.capture_error,'model_error':value.model_error,'metrics':metrics,
            'task_id':task_id,'task_found':value.task_found,'task_state':value.task_state,
            'execution_pending':value.execution_pending,'task_error':value.task_error,'result_json':value.result_json,
            'summary':f'capture={value.capture_state}, detector={value.model_state}, generation={value.generation}'}

    def set_capture(self,enable,request_id,deadline,canceled):
        status=self.get_status(deadline=deadline)
        timeout=max(100,min(30000,int((deadline-time.monotonic_ns())/1e6)))
        request=SetCapture.Request(request_id=request_id,generation=status['generation'],enable=enable,
                                   config_id='usb-yuyv-640x360-30',timeout_ms=timeout)
        reply=self.await_future(self.capture.call_async(request),deadline,canceled)
        if not reply.accepted:raise AgentError('CAPTURE_ADMISSION:'+reply.error_code)
        # Service cancellation is observation-only here; its bounded core
        # deadline still owns cleanup. No direct camera/state mutation is used.
        while True:
            task=self.get_status(reply.task_id,deadline)
            if task['task_found'] and task['task_state'] in ('DONE','FAILED','CANCELED','EXPIRED') and not task['execution_pending']:
                if task['task_state']!='DONE':raise AgentError(task['task_error'] or task['task_state'],task)
                task['task_id']=reply.task_id;return task
            if canceled.is_set():raise AgentError('CANCELED_CONTROL_CLEANUP_PENDING')
            if time.monotonic_ns()>=deadline:raise AgentError('CONTROL_DEADLINE_CLEANUP_PENDING')
            time.sleep(.02)

    def action(self,operation,request_id,deadline,canceled,snapshot_id='',prompt='',freshness_ms=500):
        status=self.get_status(deadline=deadline)
        timeout=max(1,min(30000,int((deadline-time.monotonic_ns())/1e6)))
        goal=RunInspection.Goal(request_id=request_id,generation=status['generation'],operation=operation,
            timeout_ms=timeout,snapshot_id=snapshot_id,prompt=prompt,freshness_ms=freshness_ms)
        # Admission is bounded even if the caller cancels while the reply is in
        # flight. Once a goal handle is known, cancellation follows the Action.
        handle=self.await_future(self.inspection.send_goal_async(goal),deadline)
        if not handle.accepted:raise AgentError('CORE_ACTION_BUSY_OR_INVALID')
        result_future=handle.get_result_async();cancel_sent=False;cleanup_deadline=None
        while not result_future.done():
            if canceled.is_set() or time.monotonic_ns()>=deadline:
                if not cancel_sent:
                    handle.cancel_goal_async();cancel_sent=True;cleanup_deadline=time.monotonic_ns()+12_000_000_000
                if time.monotonic_ns()>=cleanup_deadline:raise AgentError('ACTION_PHYSICAL_CLEANUP_UNCONFIRMED')
            time.sleep(.02)
        response=result_future.result();result=response.result
        current=self.get_status(result.task_id,deadline=time.monotonic_ns()+5_000_000_000)
        receipt={'task_id':result.task_id,'generation':current['generation'],'runtime_instance':current['runtime_instance'],
            'state':result.state,'error_code':result.error_code,'result_json':result.result_json,
            'execution_pending':current['execution_pending'],'ros_action_status':response.status}
        if current['execution_pending']:raise AgentError('ACTION_RESULT_BEFORE_PHYSICAL_COMPLETION',receipt)
        if response.status!=4 or not result.success:raise AgentError(result.error_code or result.state,receipt)
        data=json.loads(result.result_json)
        if data.get('task_id')!=result.task_id or data.get('generation')!=status['generation']:
            raise AgentError('CORE_RESULT_IDENTITY')
        data['runtime_instance']=status['runtime_instance'];return data

    def restore_detector(self,request_id):
        return self.action('resume_detector',request_id,time.monotonic_ns()+30_000_000_000,threading.Event())
