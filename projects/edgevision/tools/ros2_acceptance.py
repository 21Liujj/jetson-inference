#!/usr/bin/env python3
"""Real ROS2/USB acceptance. Source Humble and the owned install before running.

Images and bags stay in the private output directory, never the Git evidence ZIP.
The separately named acceptance node injects 200ms actual GPU work only for races.
"""
import argparse
import collections
import hashlib
import json
import os
import pathlib
import signal
import subprocess
import sys
import time

import rclpy
from rclpy.action import ActionClient
from rclpy.node import Node
from rclpy.qos import qos_profile_sensor_data
from sensor_msgs.msg import Image
from vision_msgs.msg import Detection2DArray
from diagnostic_msgs.msg import DiagnosticArray
from edgevision_interfaces.msg import FrameMeta
from edgevision_interfaces.srv import SetCapture, GetStatus
from edgevision_interfaces.action import RunInspection


def stop(process):
    if process.poll() is None:
        os.killpg(process.pid, signal.SIGINT)
        try:
            process.wait(timeout=25)
        except subprocess.TimeoutExpired:
            os.killpg(process.pid, signal.SIGKILL)
            process.wait(timeout=5)
            raise RuntimeError('CHILD_FORCED_KILL')
    if process.returncode not in (0, -signal.SIGINT):
        raise RuntimeError(f'CHILD_EXIT:{process.returncode}')


class Observer(Node):
    def __init__(self, out):
        super().__init__('edgevision_acceptance_observer')
        self.out = out
        self.counts = collections.Counter()
        self.image_times = []
        self.latest = {}
        self.feedback = []
        self.paired_stamps = collections.deque(maxlen=256)
        self.detection_stamps = collections.deque(maxlen=256)
        self.subscriptions_owned = [
            self.create_subscription(Image, '/edgevision/image', self.image, qos_profile_sensor_data),
            self.create_subscription(Image, '/edgevision/detection_image', self.paired, qos_profile_sensor_data),
            self.create_subscription(Detection2DArray, '/edgevision/detections', self.detection, qos_profile_sensor_data),
            self.create_subscription(FrameMeta, '/edgevision/frame_meta', self.meta, qos_profile_sensor_data),
            self.create_subscription(DiagnosticArray, '/diagnostics', self.diag, 5),
        ]
        self.capture = self.create_client(SetCapture, '/edgevision/set_capture')
        self.status = self.create_client(GetStatus, '/edgevision/get_status')
        self.action = ActionClient(self, RunInspection, '/edgevision/run_inspection')

    def image(self, message):
        assert message.encoding == 'rgb8' and message.step == message.width * 3
        assert len(message.data) == message.step * message.height
        assert message.header.frame_id == 'camera_optical_frame'
        assert abs(time.time() - message.header.stamp.sec) < 10  # publication ROS time, not monotonic
        self.counts['image'] += 1
        self.image_times.append(time.monotonic_ns())

    def paired(self, message):
        assert message.encoding == 'rgb8' and len(message.data) == message.step * message.height
        self.counts['paired_image'] += 1
        self.latest['paired_stamp'] = (message.header.stamp.sec, message.header.stamp.nanosec)
        self.paired_stamps.append(self.latest['paired_stamp'])
        if not (self.out / 'private-real-detection-frame.rgb').exists():
            (self.out / 'private-real-detection-frame.rgb').write_bytes(bytes(message.data))
            (self.out / 'private-real-detection-frame.json').write_text(json.dumps(
                {'width': message.width, 'height': message.height, 'encoding': message.encoding}) + '\n')

    def detection(self, message):
        self.counts['detections'] += 1
        self.latest['detection_stamp'] = (message.header.stamp.sec, message.header.stamp.nanosec)
        self.detection_stamps.append(self.latest['detection_stamp'])
        for detection in message.detections:
            assert detection.bbox.size_x >= 0 and detection.bbox.size_y >= 0
            for result in detection.results:
                assert 0 <= int(result.hypothesis.class_id) < 80
                assert 0 <= result.hypothesis.score <= 1

    def meta(self, message):
        assert message.schema_version == 1 and message.generation > 0  # frame IDs begin at zero
        assert message.stamp_source == 'publication_ros_time'
        assert message.host_dequeue_mono_ns > 0
        self.counts['metadata'] += 1
        self.latest['meta'] = {'source': message.source, 'generation': message.generation,
                               'frame_id': message.capture_frame_id, 'task_id': message.task_id,
                               'driver_clock': message.driver_clock, 'driver_timestamp_valid': message.driver_timestamp_valid}

    def diag(self, message):
        assert {row.name for row in message.status} == {'edgevision/capture', 'edgevision/model'}
        self.counts['diagnostics'] += 1

    def spin(self, seconds):
        end = time.monotonic() + seconds
        while time.monotonic() < end:
            rclpy.spin_once(self, timeout_sec=max(0, min(.02, end - time.monotonic())))

    def await_future(self, future, seconds=10):
        end = time.monotonic() + seconds
        while not future.done() and time.monotonic() < end:
            rclpy.spin_once(self, timeout_sec=.01)
        if not future.done():
            raise TimeoutError('ROS_FUTURE_BUDGET')
        return future.result()

    def get(self, task=0):
        request = GetStatus.Request()
        request.task_id = task
        return self.await_future(self.status.call_async(request))

    def until(self, predicate, seconds=20):
        end = time.monotonic() + seconds
        while time.monotonic() < end:
            value = predicate()
            if value:
                return value
            self.spin(.02)
        raise TimeoutError('ROS_CONDITION_BUDGET')

    def set(self, enable, request_id, generation, timeout=10000, config='usb-yuyv-640x360-30'):
        request = SetCapture.Request(request_id=request_id, generation=generation, enable=enable,
                                     config_id=config, timeout_ms=timeout)
        return self.await_future(self.capture.call_async(request))

    def task_done(self, task):
        def finished():
            status = self.get(task)
            return status if status.task_found and status.task_state in ('DONE', 'FAILED', 'EXPIRED', 'CANCELED') and not status.execution_pending else None
        return self.until(finished)

    def goal(self, request_id, generation, timeout=5000):
        self.feedback.clear()
        goal = RunInspection.Goal(request_id=request_id, generation=generation, operation='detect', timeout_ms=timeout)
        return self.await_future(self.action.send_goal_async(goal, feedback_callback=lambda msg: self.feedback.append(
            {'task_id': msg.feedback.task_id, 'state': msg.feedback.state, 'execution_pending': msg.feedback.execution_pending})))


def slow_child(out):
    rclpy.init()
    node = Node('edgevision_slow_subscriber')
    seen = []
    def callback(message):
        seen.append(time.monotonic_ns())
        time.sleep(.4)
    sub = node.create_subscription(Image, '/edgevision/image', callback, qos_profile_sensor_data)
    started = time.monotonic()
    try:
        while time.monotonic() - started < 10:
            rclpy.spin_once(node, timeout_sec=.1)
    finally:
        pathlib.Path(out).write_text(json.dumps({'messages': len(seen), 'callback_sleep_s': .4, 'elapsed_s': time.monotonic()-started}) + '\n')
        node.destroy_node()
        rclpy.shutdown()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', type=pathlib.Path)
    parser.add_argument('--acceptance-binary', type=pathlib.Path)
    parser.add_argument('--model', type=pathlib.Path)
    parser.add_argument('--labels', type=pathlib.Path)
    parser.add_argument('--device')
    parser.add_argument('--out', type=pathlib.Path)
    parser.add_argument('--slow-child')
    args = parser.parse_args()
    if args.slow_child:
        slow_child(args.slow_child)
        return
    if not all([args.binary, args.acceptance_binary, args.model, args.labels, args.device, args.out]):
        parser.error('explicit real-device paths required')
    args.out.mkdir(parents=True, exist_ok=False)
    rclpy.init()
    observer = Observer(args.out)
    runs = []
    process = None
    slow = None
    bag_process = None
    def launch(binary, label):
        nonlocal process
        argv = [str(binary.resolve()), '--ros-args', '-p', 'device:='+args.device,
                '-p', 'model:='+str(args.model.resolve()), '-p', 'labels:='+str(args.labels.resolve()),
                '-p', 'auto_start:=false', '-p', 'detection_hz:='+('0.0' if 'acceptance' in label else '5.0')]
        log = (args.out / (label+'.log')).open('wb')
        child = subprocess.Popen(argv, stdout=log, stderr=subprocess.STDOUT, start_new_session=True)
        process = child  # ownership established before any readiness assertion
        log.close()
        runs.append({'label': label, 'argv': argv, 'pid': child.pid})
        observer.until(lambda: observer.capture.service_is_ready() and observer.status.service_is_ready(), 30)
        observer.until(lambda: observer.get().model_state == 'READY', 45)
        assert observer.action.wait_for_server(timeout_sec=10)
        return child
    def record(name, **values):
        with (args.out / 'events.jsonl').open('a') as stream:
            stream.write(json.dumps({'mono_ns': time.monotonic_ns(), 'event': name, **values})+'\n')
    try:
        process = launch(args.binary, 'production')
        assert not observer.set(True, 'bad-config', 0, config='unsupported').accepted
        assert not observer.set(True, 'bad-timeout', 0, timeout=0).accepted
        assert not observer.set(True, 'ros-auto-1', 0).accepted
        start = observer.set(True, 'ros-start-real', 0)
        assert start.accepted and observer.task_done(start.task_id).task_state == 'DONE'
        duplicate = observer.set(True, 'ros-start-real', 0)
        assert duplicate.accepted and duplicate.duplicate and duplicate.task_id == start.task_id
        generation = observer.get().generation
        assert not observer.set(True, 'stale-generation', 0).accepted
        before = observer.get()
        slow = subprocess.Popen([sys.executable, str(pathlib.Path(__file__).resolve()), '--slow-child',
                                 str(args.out/'slow-subscriber.json')], start_new_session=True)
        observer.spin(11)
        slow.wait(timeout=5)
        assert slow.returncode == 0
        slow = None
        after = observer.get()
        metrics = json.loads(after.metrics_json)
        assert metrics['valid_frames'] - json.loads(before.metrics_json)['valid_frames'] >= 250
        assert metrics['pool_busy'] <= 4 and metrics['queue_depth'] <= 2
        assert observer.counts['image'] >= 30 and observer.counts['detections'] >= 20 and observer.counts['diagnostics'] >= 5
        assert len(set(observer.paired_stamps) & set(observer.detection_stamps)) >= 20
        record('live_qos_slow_subscriber', counts=dict(observer.counts), metrics=metrics)
        goal = observer.goal('ros-action-success', generation)
        assert goal.accepted
        result = observer.await_future(goal.get_result_async())
        assert result.status == 4 and result.result.success and result.result.state == 'DONE'
        data = json.loads(result.result.result_json)
        assert data['task_id'] == result.result.task_id and data['generation'] == generation
        record('action_success', task_id=data['task_id'], generation=generation, frame_id=data['frame_id'])
        for name, argv in [('topic-list',['ros2','topic','list','--no-daemon']),
                           ('topic-info',['ros2','topic','info','/edgevision/image','--verbose'])]:
            run = subprocess.run(argv, capture_output=True, text=True, timeout=15)
            (args.out/(name+'.log')).write_text(run.stdout+run.stderr)
            assert run.returncode == 0
        bag_dir = args.out / 'private-rosbag'
        with (args.out/'bag-record.log').open('wb') as bag_log:
            bag_process = subprocess.Popen(['ros2','bag','record','--max-bag-size','8388608','--max-cache-size','1048576',
                '-o',str(bag_dir),'/edgevision/image','/edgevision/frame_meta','/edgevision/detections','/diagnostics'],
                stdout=bag_log,stderr=subprocess.STDOUT,start_new_session=True)
        observer.spin(4)
        stop(bag_process)
        bag_process = None
        bag_files = [p for p in bag_dir.rglob('*') if p.is_file()]
        assert sum(p.stat().st_size for p in bag_files) <= 24*1024*1024
        assert any(p.suffix == '.db3' for p in bag_files)
        (args.out/'bag-file-manifest.json').write_text(json.dumps([
            {'file':p.name,'bytes':p.stat().st_size,'sha256':hashlib.sha256(p.read_bytes()).hexdigest()} for p in bag_files],indent=2)+'\n')
        info = subprocess.run(['ros2','bag','info',str(bag_dir)],capture_output=True,text=True,timeout=15)
        (args.out/'bag-info.log').write_text(info.stdout+info.stderr)
        assert info.returncode == 0
        stop_reply = observer.set(False, 'ros-stop-real', generation)
        assert stop_reply.accepted and observer.task_done(stop_reply.task_id).task_state == 'DONE'
        assert not observer.get().resources_live and observer.get().pool_busy == 0
        failed = observer.goal('ros-action-no-capture', observer.get().generation)
        assert failed.accepted
        failure = observer.await_future(failed.get_result_async())
        assert failure.status == 6 and failure.result.state == 'FAILED' and not failure.result.success
        record('action_failure', state=failure.result.state, error=failure.result.error_code)
        replay_sub = observer.create_subscription(Image,'/edgevision/replayed_image',
            lambda message: observer.counts.update(replayed_image=1) if len(message.data)==message.step*message.height else None,
            qos_profile_sensor_data)
        with (args.out/'bag-play.log').open('wb') as log:
            bag_process = subprocess.Popen(['ros2','bag','play',str(bag_dir),'--remap',
                '/edgevision/image:=/edgevision/replayed_image','/edgevision/frame_meta:=/edgevision/replayed_meta',
                '/edgevision/detections:=/edgevision/replayed_detections','/diagnostics:=/edgevision/replayed_diagnostics'],
                stdout=log,stderr=subprocess.STDOUT,start_new_session=True)
        observer.until(lambda: bag_process.poll() is not None,20)
        assert bag_process.returncode == 0 and observer.counts['replayed_image'] > 0
        bag_process = None
        observer.destroy_subscription(replay_sub)
        record('bag_replayed',images=observer.counts['replayed_image'],bytes=sum(p.stat().st_size for p in bag_files))
        stop(process)
        process = None
        observer.spin(1)
        # The second executable has explicit test-only actual GPU delay. Product
        # parameters/services provide no switch for enabling this injection.
        process = launch(args.acceptance_binary, 'gpu-acceptance-200ms')
        started = observer.set(True, 'acceptance-start', 0)
        assert started.accepted and observer.task_done(started.task_id).task_state == 'DONE'
        generation = observer.get().generation
        # Wait out an internal auto job, then reserve the Action so auto admission pauses.
        goal = observer.goal('ros-action-cancel-gpu', generation)
        assert goal.accepted
        observer.until(lambda: bool(observer.feedback))
        cancel_task = observer.feedback[-1]['task_id']
        def own_gpu_pending():
            metrics = json.loads(observer.get(cancel_task).metrics_json)
            return metrics if metrics['physical_busy'] and metrics['gpu_task_id'] == cancel_task and metrics['gpu_submitted_mono_ns'] else None
        pending = observer.until(own_gpu_pending)
        canceled = observer.await_future(goal.cancel_goal_async())
        assert canceled.goals_canceling
        cancel_result = observer.await_future(goal.get_result_async())
        assert cancel_result.status == 5 and cancel_result.result.state == 'CANCELED'
        final = observer.get(cancel_result.result.task_id)
        assert not final.execution_pending
        record('action_cancel', task_id=cancel_result.result.task_id, state=cancel_result.result.state, pending_before_cancel=pending)
        gpu_before = json.loads(observer.get().metrics_json)['gpu_submissions']
        expired_goal = observer.goal('ros-action-deadline', generation, 50)
        assert expired_goal.accepted
        expired = observer.await_future(expired_goal.get_result_async())
        assert expired.status == 6 and expired.result.state == 'EXPIRED'
        assert not observer.get(expired.result.task_id).execution_pending
        assert json.loads(observer.get().metrics_json)['gpu_submissions'] == gpu_before + 1
        record('action_deadline', task_id=expired.result.task_id, state=expired.result.state)
        final_stop = observer.set(False, 'acceptance-stop', generation)
        assert final_stop.accepted and observer.task_done(final_stop.task_id).task_state == 'DONE'
        assert not observer.get().resources_live and observer.get().pool_busy == 0
        stop(process)
        process = None
        summary = {'schema_version':1,'simulated':False,'pass':True,'counts':dict(observer.counts),
                   'node_runs':runs,'feedback':observer.feedback,'images_and_bags_private':True,
                   'limits':'functional acceptance; not overhead matrix or final-stack endurance'}
        (args.out/'summary.json').write_text(json.dumps(summary,indent=2)+'\n')
        print(json.dumps({k:summary[k] for k in ('pass','counts')}))
    finally:
        if bag_process is not None and bag_process.poll() is None:
            stop(bag_process)
        if slow is not None and slow.poll() is None:
            stop(slow)
        if process is not None:
            stop(process)
        observer.destroy_node()
        rclpy.shutdown()


if __name__ == '__main__':
    main()
