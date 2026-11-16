#include "control_fixture.h"
#include <cstdio>
static TaskAdmission running(ControlRouter& router, Fixture& backend, CaptureConfig config = {}) {
  auto request = start("start", 0, 1000000000);
  if (config.device.empty()) config.device = "/dev/v4l/by-id/explicit-cpu-fixture";
  config.runtime_retry_delay_ms = 50; config.runtime_recovery_budget_ms = 500;
  request.capture = config;
  auto task = router.submit(request, 1); assert(task.accepted); router.poll(2); backend.first(1); router.poll(3);
  assert(router.status().capture == CaptureState::Running && router.query(task.task_id)->state == TaskState::Done);
  return task;
}
static void fault(Fixture& backend, uint64_t generation, bool retryable = true) {
  CaptureEvent e; e.kind = CaptureEvent::Kind::Fault; e.generation = generation;
  e.runtime_retryable = retryable; e.error_code = "DEVICE_UNAVAILABLE"; backend.events.push_back(e);
}
int main() {
  {
    Fixture backend; ControlRouter router(backend); auto original = running(router, backend);
    fault(backend, 1); router.poll(100);
    assert(router.status().capture == CaptureState::Stopping && backend.starts == 1);
    router.poll(1000); assert(backend.starts == 1); // cannot reopen while the old device/leases are live
    backend.stopped(1); router.poll(2000);
    assert(router.status().capture == CaptureState::Recovering && !router.status().backend_resources_live);
    router.poll(50001999); assert(backend.starts == 1);
    router.poll(50002000); assert(backend.starts == 2 && router.status().generation == 2);
    assert(router.status().capture == CaptureState::Recovering && router.status().runtime_retries == 1);
    backend.first(1); backend.stopped(1); backend.first(2); router.poll(50002001);
    assert(router.status().capture == CaptureState::Running && router.status().stale_events == 2);
    assert(router.status().fault_detected_mono_ns == 100 && router.status().recovered_mono_ns == 50002001);
    auto history = router.query(original.task_id);
    assert(history->state == TaskState::Done && history->execution_generation == 1 && history->result.find("\"generation\":1") != std::string::npos);
    auto old_stop = stop("stale-stop", 1); old_stop.deadline_mono_ns = 1000000000;
    assert(router.submit(old_stop, 50002002).error == AdmissionError::StaleGeneration);
  }
  for (bool cancel_stop : {false, true}) {
    Fixture backend; ControlRouter router(backend); running(router, backend);
    fault(backend, 1); backend.stopped(1); router.poll(100);
    auto r = stop("stop-waiting", 1); r.deadline_mono_ns = 1000000000;
    auto s = router.submit(r, 101); assert(s.accepted);
    if (cancel_stop) router.cancel(s.task_id, 102);
    router.poll(103); router.poll(50000100);
    if (cancel_stop) assert(backend.starts == 2 && router.status().capture == CaptureState::Recovering);
    else assert(backend.starts == 1 && router.status().capture == CaptureState::Idle && router.query(s.task_id)->state == TaskState::Done);
  }
  {
    Fixture backend; ControlRouter router(backend); running(router, backend);
    fault(backend, 1); backend.stopped(1); router.poll(100); router.poll(50000100);
    auto r = stop("stop-opening", 2); r.deadline_mono_ns = 1000000000;
    auto s = router.submit(r, 50000101); assert(s.accepted);
    backend.first(2); router.poll(50000102); // queued stop wins over an arriving recovery first frame
    assert(router.status().capture == CaptureState::Stopping && router.status().recovered_mono_ns == 0);
    assert(router.query(s.task_id)->execution_pending);
    backend.stopped(2); router.poll(50000103);
    assert(router.status().capture == CaptureState::Idle && router.query(s.task_id)->state == TaskState::Done);
  }
  {
    Fixture backend; ControlRouter router(backend); running(router, backend);
    fault(backend, 1); router.poll(100); // cleanup is still physically pending
    auto r = stop("stop-fault-cleanup", 1); r.deadline_mono_ns = 1000000000;
    auto s = router.submit(r, 101); assert(s.accepted); router.poll(102);
    assert(router.query(s.task_id)->state == TaskState::Running && router.status().backend_resources_live);
    backend.stopped(1); router.poll(103); router.poll(100000000);
    assert(router.status().capture == CaptureState::Idle && router.query(s.task_id)->state == TaskState::Done && backend.starts == 1);
  }
  {
    Fixture backend; ControlRouter router(backend); auto original = running(router, backend);
    fault(backend, 1); backend.stopped(1); router.poll(100); router.poll(50000100);
    router.poll(500000100); // budget expires while a reopened backend has no valid frame
    assert(router.status().capture == CaptureState::Stopping && router.status().backend_resources_live);
    assert(router.status().last_error == "RUNTIME_RECOVERY_TIMEOUT");
    backend.first(2); router.poll(500000101); assert(router.status().capture == CaptureState::Stopping);
    backend.stopped(2); router.poll(500000102);
    assert(router.status().capture == CaptureState::Error && !router.status().backend_resources_live);
    assert(router.query(original.task_id)->state == TaskState::Done);
  }
  {
    Fixture backend; ControlRouter router(backend); CaptureConfig c; c.runtime_retry_limit = 2;
    running(router, backend, c);
    uint64_t t = 100;
    for (uint64_t generation = 1; generation <= 3; ++generation) {
      fault(backend, generation); backend.stopped(generation); router.poll(t);
      if (generation < 3) { t += uint64_t(50000000) << (generation - 1); router.poll(t); }
      ++t;
    }
    assert(router.status().capture == CaptureState::Error && backend.starts == 3 && router.status().runtime_retries == 2);
  }
  {
    Fixture backend; ControlRouter router(backend); CaptureConfig c; c.runtime_retry_limit = 1;
    running(router, backend, c);
    fault(backend, 1); backend.stopped(1); router.poll(100); router.poll(50000100);
    backend.first(2); router.poll(50000101);
    assert(router.status().capture == CaptureState::Running);
    fault(backend, 2); backend.stopped(2); router.poll(60000000);
    assert(router.status().capture == CaptureState::Error && backend.starts == 2); // success does not replenish a flapping camera's budget
  }
  for (unsigned which = 0; which < 3; ++which) {
    Fixture backend; ControlRouter router(backend); CaptureConfig c;
    if (which == 0) c.runtime_retry_limit = 0;
    if (which == 1) { c.device = "/dev/video0"; c.allow_unstable_device = true; }
    running(router, backend, c);
    fault(backend, 1, which != 2); backend.stopped(1); router.poll(100);
    assert(router.status().capture == CaptureState::Error && backend.starts == 1);
  }
  {
    Fixture backend; ControlRouter router(backend); running(router, backend);
    fault(backend, 1); backend.stopped(1); router.poll(100);
    auto r = start("conflicting-start", 1, 1000000000);
    assert(router.submit(r, 101).accepted);
    router.poll(50000100); // a rejected queued start cannot swallow the automatic reopen action
    assert(backend.starts == 2 && router.status().backend_resources_live && router.status().generation == 2);
  }
  std::puts("PASS runtime recovery: physical cleanup, stable identity, generation/history, stop priority, deadline and total retry budget; CPU fixture only");
}
