#include "edgevision/v4l2_backend.h"
#include "v4l2_fixture.h"
#include <chrono>
#include <thread>
#include <cstdio>
using namespace edgevision;
static uint64_t now() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
static ControlRequest start(std::string id, uint64_t generation = 0) {
  CaptureConfig c; c.device = "/dev/v4l/by-id/explicit-worker-fixture"; c.fourcc = V4L2_PIX_FMT_MJPEG;
  return {std::move(id), generation, now() + 3000000000ULL, TaskCommand::StartCapture, c};
}
template<class Predicate> void until(ControlRouter& router, Predicate done) {
  auto end = now() + 2000000000ULL;
  do { router.poll(now()); if (done()) return; std::this_thread::sleep_for(std::chrono::milliseconds(1)); } while (now() < end);
  assert(false && "backend contract timed out");
}
int main() {
  {
    auto io = std::make_shared<Fixture>(); V4L2Backend backend(io); ControlRouter router(backend);
    auto a = router.submit(start("start"), now()); assert(a.accepted);
    until(router, [&] { return router.status().capture == CaptureState::Running; });
    assert(router.query(a.task_id)->state == TaskState::Done);
    FrameLease held; assert(backend.consume(held, 1000) && held.valid() && held.state() == SlotState::Ready);
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    auto snapshot = backend.snapshot(); assert(snapshot.queue_depth <= 2 && snapshot.pool_busy <= 4 && snapshot.queue_replaced > 0);
    ControlRequest s{"stop", 1, now() + 3000000000ULL, TaskCommand::StopCapture, {}};
    auto b = router.submit(s, now()); assert(b.accepted); router.poll(now());
    until(router, [&] { return backend.snapshot().cleanup_pending; });
    std::this_thread::sleep_for(std::chrono::milliseconds(20)); router.poll(now());
    assert(router.status().capture == CaptureState::Stopping && router.query(b.task_id)->execution_pending);
    assert(io->streamoffs.load() == 0 && held->data[0] == 42); // stop waits for the real lease, not a timer
    held.release();
    until(router, [&] { return router.status().capture == CaptureState::Idle; });
    assert(router.query(b.task_id)->state == TaskState::Done && io->streamoffs.load() == 1);
    assert(io->closes == 1 && io->maps == io->unmaps && !backend.snapshot().worker_active);
    auto c = router.submit(start("restart", 1), now()); assert(c.accepted);
    until(router, [&] { return router.status().capture == CaptureState::Running; });
    assert(router.status().generation == 2 && router.query(c.task_id)->state == TaskState::Done);
    s.request_id = "stop-2"; s.generation = 2; s.deadline_mono_ns = now() + 3000000000ULL;
    assert(router.submit(s, now()).accepted);
    until(router, [&] { return router.status().capture == CaptureState::Idle; });
    assert(io->opens == 2 && io->closes == 2 && io->maps == io->unmaps);
  }
  {
    auto io = std::make_shared<Fixture>(); io->no_frames = true;
    V4L2Backend backend(io); ControlRouter router(backend);
    auto a = router.submit(start("no-frame"), now()); router.poll(now());
    until(router, [&] { return io->waiting.load(); });
    assert(router.status().capture == CaptureState::Starting);
    assert(router.cancel(a.task_id, now()));
    until(router, [&] { return router.status().capture == CaptureState::Idle; });
    assert(router.query(a.task_id)->state == TaskState::Canceled && !router.query(a.task_id)->execution_pending);
    assert(io->closes == 1 && io->event_closes == 1 && io->streamoffs.load() == 1);
  }
  {
    auto io = std::make_shared<Fixture>(); io->fail_open = true;
    V4L2Backend backend(io); ControlRouter router(backend); auto a = router.submit(start("missing"), now());
    until(router, [&] { return router.status().capture == CaptureState::Error && !router.status().backend_resources_live; });
    assert(router.query(a.task_id)->state == TaskState::Failed && backend.snapshot().error.find("OPEN") == 0);
    assert(io->maps == 0 && io->closes == 0);
  }
  {
    auto io = std::make_shared<Fixture>(); io->fail_request = VIDIOC_S_FMT; io->fail_once = true;
    V4L2Backend backend(io); ControlRouter router(backend);
    auto r = start("transient-eio"); r.capture->retry_delay_ms = 10;
    auto a = router.submit(r, now());
    until(router, [&] { return router.status().capture == CaptureState::Running; });
    assert(io->opens == 2 && router.status().generation == 2 && router.status().start_retries == 1);
    assert(router.query(a.task_id)->request.generation == 0 && router.query(a.task_id)->execution_generation == 2);
    assert(router.query(a.task_id)->state == TaskState::Done);
    ControlRequest s{"stop-retry", 2, now() + 3000000000ULL, TaskCommand::StopCapture, {}};
    assert(router.submit(s, now()).accepted);
    until(router, [&] { return router.status().capture == CaptureState::Idle; });
    assert(io->opens == io->closes && io->maps == io->unmaps);
  }
  {
    auto io = std::make_shared<Fixture>(); V4L2Backend backend(io); ControlRouter router(backend);
    auto request = start("disconnect"); request.capture->runtime_retry_delay_ms = 50;
    auto original = router.submit(request, now());
    until(router, [&] { return router.status().capture == CaptureState::Running; });
    FrameLease held; assert(backend.consume(held, 1000));
    io->disconnected.store(true);
    until(router, [&] { return router.status().capture == CaptureState::Stopping; });
    assert(io->opens == 1 && io->streamoffs.load() == 0); // retained lease blocks physical reopen
    held.release();
    until(router, [&] { return router.status().runtime_retries == 1 && !router.status().backend_resources_live; });
    // One real worker reopen attempted while the injected device was absent.
    io->disconnected.store(false);
    until(router, [&] { return router.status().capture == CaptureState::Running; });
    assert(router.status().generation == 3 && router.status().runtime_retries == 2);
    assert(router.query(original.task_id)->state == TaskState::Done && router.query(original.task_id)->execution_generation == 1);
    ControlRequest stop_request{"stop-after-recovery", 3, now() + 3000000000ULL, TaskCommand::StopCapture, {}};
    assert(router.submit(stop_request, now()).accepted);
    until(router, [&] { return router.status().capture == CaptureState::Idle; });
    assert(io->opens == 3 && io->closes == 2 && io->maps == io->unmaps);
  }
  std::puts("PASS real capture worker/router/queue/lease-drain/restart/cancel/open-failure contracts with explicit syscall fixture");
}
