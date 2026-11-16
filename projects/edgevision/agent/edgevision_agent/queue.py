import dataclasses
import queue
import threading
import time
import uuid
from .schema import AgentError,request_key


@dataclasses.dataclass
class Job:
    request_id:str
    goal:str
    timeout_s:float
    canceled:threading.Event=dataclasses.field(default_factory=threading.Event)
    state:str='QUEUED'
    result:dict|None=None
    error_code:str=''
    deadline_ns:int=0


class AgentQueue:
    """One active request, one pending request, 64 explicit retained identities."""
    def __init__(self,runner):
        self.runner=runner;self.pending=queue.Queue(maxsize=1);self.records={};self.lock=threading.Lock()
        self.closing=False;self.worker=threading.Thread(target=self._work,daemon=True);self.worker.start()

    def submit(self,goal,request_id=None,timeout_s=180):
        request_id=request_id or uuid.uuid4().hex;request_key(request_id)
        if not goal.strip() or len(goal.encode())>800 or not 1<=timeout_s<=300:raise AgentError('AGENT_REQUEST_BUDGET')
        with self.lock:
            old=self.records.get(request_id)
            if old:
                if old.goal!=goal or old.timeout_s!=timeout_s:raise AgentError('REQUEST_ID_CONFLICT')
                return old,True
            if self.closing:raise AgentError('AGENT_SHUTTING_DOWN')
            if len(self.records)>=64:raise AgentError('AGENT_RECORD_BUDGET')
            job=Job(request_id,goal,timeout_s,deadline_ns=time.monotonic_ns()+int(timeout_s*1e9))
            try:self.pending.put_nowait(job)
            except queue.Full:raise AgentError('AGENT_QUEUE_FULL')
            self.records[request_id]=job;return job,False

    def query(self,request_id):
        with self.lock:
            job=self.records.get(request_id)
            if not job:return None
            return {'request_id':job.request_id,'state':job.state,'error_code':job.error_code,'result':job.result}

    def cancel(self,request_id):
        with self.lock:
            job=self.records.get(request_id)
            if not job:return False
            job.canceled.set()
            if job.state=='QUEUED':job.state='CANCELED'
            return True

    def _work(self):
        while True:
            try:job=self.pending.get(timeout=.1)
            except queue.Empty:
                with self.lock:
                    if self.closing:return
                continue
            with self.lock:job.state='CANCELED' if job.canceled.is_set() else 'RUNNING'
            try:
                if not job.canceled.is_set():
                    remaining=(job.deadline_ns-time.monotonic_ns())/1e9
                    if remaining<1:
                        with self.lock:job.state='EXPIRED';job.error_code='QUEUED_DEADLINE_EXCEEDED'
                        continue
                    result=self.runner.run(job.goal,job.request_id,remaining,job.canceled)
                    with self.lock:job.result=result;job.state=result['status'];job.error_code=result.get('error_code','')
            except Exception as error:
                with self.lock:job.state='FAILED';job.error_code=str(error)[:256]
            finally:self.pending.task_done()

    def close(self):
        with self.lock:
            self.closing=True
            for job in self.records.values():
                if job.state in ('RUNNING','QUEUED'):job.canceled.set()
        self.worker.join(timeout=50)
        if self.worker.is_alive():raise AgentError('AGENT_PHYSICAL_CLEANUP_UNCONFIRMED')
