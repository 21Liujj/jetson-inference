import time
from .schema import AgentError

COUNTERS=('valid_frames','pool_drop','ready_drop','sequence_gaps','gpu_submissions','gpu_completions')


def sample_window(client,window_s,deadline,canceled):
    """Bounded forward observation, never relabel lifetime counters as history."""
    if window_s!=10:raise AgentError('METRICS_WINDOW_NOT_REGISTERED')
    started=time.monotonic_ns();finish=started+int(window_s*1e9)
    if finish+500_000_000>deadline:raise AgentError('METRICS_DEADLINE_BUDGET')
    first=client.get_status(deadline=deadline);last=first;samples=1
    queue_max=first['queue_depth'];pool_max=first['pool_busy']
    while time.monotonic_ns()<finish:
        if canceled.wait(min(.25,max(0,(finish-time.monotonic_ns())/1e9))):raise AgentError('CANCELED')
        last=client.get_status(deadline=deadline);samples+=1
        if samples>42:raise AgentError('METRICS_SAMPLE_BUDGET')
        if last['generation']!=first['generation']:raise AgentError('METRICS_GENERATION_CHANGED')
        queue_max=max(queue_max,last['queue_depth']);pool_max=max(pool_max,last['pool_busy'])
    deltas={}
    for name in COUNTERS:
        a=first['metrics'].get(name);b=last['metrics'].get(name)
        if type(a) is int and type(b) is int and b>=a:deltas[name]=b-a
    finished=time.monotonic_ns()
    return {'runtime_instance':last['runtime_instance'],'generation':last['generation'],
        'window_s':window_s,'observation':'forward_sampled_window','samples':samples,
        'started_mono_ns':started,'finished_mono_ns':finished,'counter_deltas':deltas,
        'queue_depth_max_sampled':queue_max,'pool_busy_max_sampled':pool_max,
        'last_metrics':last['metrics'],'summary':f'实际采样 {(finished-started)/1e9:.3f} 秒；计数增量 {deltas}；队列采样最大值 {queue_max}。不足以单独确认内核丢帧原因，需要对应 trace。'}
