#include "edgevision/control_router.h"
#include <cassert>
#include <cstdio>
#include <deque>
#include <functional>
#include <future>
#include <stdexcept>
using namespace edgevision;

// Explicit CPU fixture. No device, driver, image, CUDA or model is opened.
struct Fixture final : CaptureBackend {
  unsigned starts = 0, stops = 0;
  bool reject = false, throw_start = false, throw_event = false;
  uint64_t last_generation = 0;
  std::function<void()> inspect;
  std::deque<CaptureEvent> events;
  BackendStartReply request_start(const CaptureConfig&, uint64_t generation) override {
    ++starts; last_generation = generation; if (inspect) inspect();
    if (throw_start) throw std::runtime_error("partial start fixture");
    return {!reject, reject ? "NO_DEVICE" : ""};
  }
  void request_stop(uint64_t generation) noexcept override { assert(generation == last_generation); ++stops; if (inspect) inspect(); }
  std::optional<CaptureEvent> poll_event() override {
    if (throw_event) { throw_event = false; throw std::runtime_error("poll fixture"); }
    if (inspect) inspect(); if (events.empty()) return std::nullopt;
    auto event = events.front(); events.pop_front(); return event;
  }
  void first(uint64_t generation, bool stream_on = true) {
    CaptureEvent e; e.kind = CaptureEvent::Kind::FirstFrame; e.generation = generation; e.stream_on = stream_on;
    e.frame.session_generation = generation; e.frame.frame_id = 47; e.frame.width = 640; e.frame.height = 360;
    e.frame.stride = 1280; e.frame.bytesused = 460800; e.frame.host_dequeue_mono_ns = 1;
    e.frame.pixfmt_fourcc = make_fourcc('Y','U','Y','V'); events.push_back(e);
  }
  void stopped(uint64_t generation) { CaptureEvent e; e.kind = CaptureEvent::Kind::Stopped; e.generation = generation; events.push_back(e); }
};
static ControlRequest start(std::string id, uint64_t generation = 0, uint64_t deadline = 1000) {
  CaptureConfig c; c.device = "/dev/v4l/by-id/explicit-cpu-fixture";
  return {std::move(id), generation, deadline, TaskCommand::StartCapture, c};
}
static ControlRequest stop(std::string id, uint64_t generation) {
  return {std::move(id), generation, 1000, TaskCommand::StopCapture, std::nullopt};
}
int main() {
  {
    Fixture backend; ControlRouter router(backend);
    backend.inspect = [&] { (void)router.status(); }; // reentry detects backend calls made while holding router lock
    auto bad = start("bad"); bad.capture->device = "/dev/video0";
    assert(router.submit(bad, 1).error == AdmissionError::InvalidParameters);
    bad.capture->allow_unstable_device = true; assert(ControlRouter::valid_config(*bad.capture));
    bad.capture->width = 641; assert(!ControlRouter::valid_config(*bad.capture));
    bad = start("empty-device"); bad.capture->device = "/dev/v4l/by-id/";
    assert(router.submit(bad, 1).error == AdmissionError::InvalidParameters);
    bad = start("budget"); bad.capture->memory_budget_bytes = 10;
    assert(router.submit(bad, 1).error == AdmissionError::InvalidParameters);
    auto unsupported = start("unsupported"); unsupported.command = TaskCommand::DescribeSnapshot;
    assert(router.submit(unsupported, 1).error == AdmissionError::UnsupportedCommand);
    auto a = router.submit(start("a"), 10); assert(a.accepted && backend.starts == 0);
    router.poll(20);
    assert(router.status().capture == CaptureState::Starting && backend.starts == 1);
    assert(router.query(a.task_id)->state == TaskState::Running);
    assert(router.query(a.task_id)->request.generation == 0 && router.query(a.task_id)->execution_generation == 1);
    auto duplicate = router.submit(start("a", 0, 9999), 21);
    assert(duplicate.accepted && duplicate.duplicate && duplicate.task_id == a.task_id);
    assert(router.query(a.task_id)->request.deadline_mono_ns == 1000);
    auto conflict = start("a"); conflict.capture->width = 800;
    assert(router.submit(conflict, 22).error == AdmissionError::Conflict);
    backend.first(1); router.poll(30);
    assert(router.status().capture == CaptureState::Running && router.status().model == ModelState::Unloaded);
    assert(router.query(a.task_id)->state == TaskState::Done && !router.query(a.task_id)->execution_pending);
    assert(router.query(a.task_id)->result.find("47") != std::string::npos);
    auto b = router.submit(stop("b", 1), 31); assert(b.accepted); router.poll(32);
    assert(router.status().capture == CaptureState::Stopping && router.query(b.task_id)->state == TaskState::Running);
    assert(router.status().backend_cleanup_pending && backend.stops == 1);
    backend.stopped(1); router.poll(33);
    assert(router.status().capture == CaptureState::Idle && !router.status().backend_resources_live);
    assert(router.query(b.task_id)->state == TaskState::Done);
    auto c = router.submit(start("c", 1), 34); assert(c.accepted); router.poll(35);
    backend.first(1); backend.first(2); router.poll(36);
    assert(router.status().capture == CaptureState::Running && router.status().generation == 2 && router.status().stale_events == 1);
    assert(router.submit(stop("stale", 1), 37).error == AdmissionError::StaleGeneration);
  }
  // Cancel/expire a STARTING request: late first frames cannot publish RUNNING;
  // the active task retains its resources until actual cleanup acknowledgement.
  for (bool canceled : {false, true}) {
    Fixture backend; ControlRouter router(backend);
    auto a = router.submit(start("a", 0, 10), 1); router.poll(2);
    if (canceled) assert(router.cancel(a.task_id, 3));
    backend.first(1); router.poll(canceled ? 4 : 11);
    assert(router.status().capture == CaptureState::Stopping && backend.stops == 1);
    assert(router.query(a.task_id)->execution_pending);
    assert(router.query(a.task_id)->state == (canceled ? TaskState::Canceled : TaskState::Expired));
    backend.stopped(1); router.poll(12);
    assert(router.status().capture == CaptureState::Idle && !router.query(a.task_id)->execution_pending);
    assert(router.query(a.task_id)->result.empty());
  }
  {
    Fixture backend; ControlRouter router(backend);
    auto a = router.submit(start("a"), 1); router.poll(2);
    auto b = router.submit(stop("stop-during-start", 1), 3); assert(b.accepted);
    assert(router.query(b.task_id)->state == TaskState::Queued);
    router.poll(4); // no frame ever arrived; stop may not wait for the start deadline
    assert(router.query(a.task_id)->state == TaskState::Canceled && router.status().capture == CaptureState::Stopping);
    assert(router.query(a.task_id)->execution_pending && router.query(b.task_id)->state == TaskState::Queued);
    assert(backend.stops == 1);
    backend.stopped(1); router.poll(5); assert(router.query(b.task_id)->state == TaskState::Done);
    assert(!router.query(a.task_id)->execution_pending);
  }
  {
    Fixture backend; ControlRouter router(backend);
    auto a = router.submit(start("a"), 1); router.poll(2);
    auto b = router.submit(stop("canceled-stop", 1), 3); assert(b.accepted);
    assert(router.cancel(b.task_id, 4)); router.poll(5);
    assert(router.status().capture == CaptureState::Starting && backend.stops == 0);
    backend.first(1); router.poll(6);
    assert(router.query(a.task_id)->state == TaskState::Done && router.status().capture == CaptureState::Running);
  }
  {
    Fixture backend; ControlRouter router(backend);
    auto a = router.submit(start("a"), 1); router.poll(2); backend.first(1); router.poll(3);
    auto b = router.submit(stop("stop", 1), 4); router.poll(5);
    assert(router.cancel(b.task_id, 6)); router.poll(7);
    assert(router.status().capture == CaptureState::Stopping && router.query(b.task_id)->execution_pending);
    backend.stopped(1); router.poll(8);
    assert(router.query(b.task_id)->state == TaskState::Canceled && !router.query(b.task_id)->execution_pending);
    assert(router.status().capture == CaptureState::Idle && router.query(a.task_id)->state == TaskState::Done);
  }
  {
    Fixture backend; backend.reject = true; ControlRouter router(backend);
    auto a = router.submit(start("a"), 1); router.poll(2);
    assert(router.status().capture == CaptureState::Error && router.status().last_error == "NO_DEVICE");
    assert(router.query(a.task_id)->state == TaskState::Failed && !router.status().backend_resources_live);
  }
  for (bool exception : {false, true}) {
    Fixture backend; backend.throw_start = exception; ControlRouter router(backend);
    auto a = router.submit(start("a"), 1); router.poll(2);
    if (!exception) { backend.first(1, false); router.poll(3); }
    assert(router.status().capture == CaptureState::Stopping && router.query(a.task_id)->execution_pending);
    backend.stopped(1); router.poll(4);
    assert(router.status().capture == CaptureState::Error && router.query(a.task_id)->state == TaskState::Failed);
  }
  {
    Fixture backend; ControlRouter router(backend);
    auto a = router.submit(start("a"), 1); router.poll(2); backend.first(1); router.poll(3);
    assert(router.query(a.task_id)->state == TaskState::Done);
    backend.throw_event = true; router.poll(4);
    assert(router.status().capture == CaptureState::Stopping && router.status().last_error == "BACKEND_EVENT_EXCEPTION");
    assert(backend.stops == 1 && router.status().backend_cleanup_pending);
    backend.stopped(1); router.poll(5);
    assert(router.status().capture == CaptureState::Error && !router.status().backend_resources_live);
    assert(router.query(a.task_id)->state == TaskState::Done); // a completed start remains historical fact
  }
  {
    Fixture backend; ControlRouter router(backend); router.poll(1);
    auto other = std::async(std::launch::async, [&] { try { router.poll(2); return false; } catch (const std::logic_error&) { return true; } });
    assert(other.get());
  }
  std::puts("PASS ControlRouter config/first-frame/stop/generation/cancel/exception/owner; CPU fixture only");
}
