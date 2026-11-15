#include "edgevision/v4l2_capture.h"
#include <linux/videodev2.h>
#include <poll.h>
#include <sys/eventfd.h>
#include <sys/mman.h>
#include <unistd.h>
#include <cassert>
#include <cerrno>
#include <chrono>
#include <future>
#include <thread>
#include <cstdio>
using namespace edgevision;
#include "v4l2_fixture.h"
static CaptureConfig config() {
  CaptureConfig c; c.device = "/dev/v4l/by-id/explicit-syscall-fixture"; c.fourcc = V4L2_PIX_FMT_MJPEG; return c;
}
static Frame slot(uint32_t size = 1024) { Frame f; f.data.reserve(size); return f; }
int main() {
  {
    auto io = std::make_shared<Fixture>(); V4L2Capture c(io); auto cfg = config(); cfg.width = 1280; cfg.height = 720;
    assert(c.open(cfg.device) && c.start(cfg, 7));
    assert(c.format().width == 640 && c.format().height == 360 && c.format().interval_den == 30);
    assert(c.format().mapped_bytes == 4096 && c.format().pool_bytes == 4096);
    auto f = slot(); assert(c.read(&f) == V4L2Capture::Read::Frame && f.valid && f.data[0] == 42);
    assert(f.bytesused == f.data.size() && f.session_generation == 7 && f.host_dequeue_mono_ns != *f.driver_timestamp_ns);
    assert(f.driver_timestamp_ns == 1000500000 && f.driver_timestamp_clock == FrameMeta::TimestampClock::Monotonic);
    assert(f.driver_timestamp_source == FrameMeta::TimestampSource::StartOfExposure);
    c.close(); c.close(); assert(c.cleanup_complete() && io->maps == io->unmaps && io->closes == 1 && io->event_closes == 1);
  }
  for (unsigned which = 0; which < 5; ++which) {
    auto io = std::make_shared<Fixture>();
    if (which == 0) io->width = 0;
    if (which == 1) io->size = 0;
    if (which == 2) io->multi_only = true;
    if (which == 3) io->fail_request = VIDIOC_G_FMT;
    if (which == 4) io->size = 65u * 1024 * 1024;
    V4L2Capture c(io); assert(c.open(config().device) && !c.start(config(), 1));
    c.close(); assert(c.cleanup_complete() && io->maps == 0 && io->closes == 1);
  }
  {
    auto io = std::make_shared<Fixture>(); io->bad_map = 1; V4L2Capture c(io);
    assert(c.open(config().device) && !c.start(config(), 1)); c.close();
    assert(io->maps == 2 && io->unmaps == 1 && io->releases == 1 && c.cleanup_complete());
  }
  {
    auto io = std::make_shared<Fixture>(); io->maplen = 1024 * 1024; auto cfg = config(); cfg.memory_budget_bytes = 4u * 1024 * 1024;
    V4L2Capture c(io); assert(c.open(cfg.device) && !c.start(cfg, 1));
    assert(c.last_error() == "MAPPED_MEMORY_BUDGET"); c.close(); assert(io->maps == io->unmaps && c.cleanup_complete());
  }
  for (unsigned which = 0; which < 3; ++which) {
    auto io = std::make_shared<Fixture>(); V4L2Capture c(io); assert(c.open(config().device) && c.start(config(), 1));
    if (which == 0) io->bytes = 0; if (which == 1) io->bytes = 1025; if (which == 2) io->flags |= V4L2_BUF_FLAG_ERROR;
    auto f = slot(); auto q = io->qbufs;
    assert(c.read(&f) == V4L2Capture::Read::InvalidPayload && !f.valid && io->qbufs == q + 1);
    assert(c.counters().invalid_payloads == 1);
  }
  {
    auto io = std::make_shared<Fixture>(); V4L2Capture c(io); assert(c.open(config().device) && c.start(config(), 1));
    auto q = io->qbufs; assert(c.read(nullptr) == V4L2Capture::Read::NoSlot && io->qbufs == q + 1);
    Frame f; assert(c.read(&f) == V4L2Capture::Read::Fault && !f.valid && c.last_error() == "SLOT_CAPACITY_NOT_RESERVED");
    assert(c.counters().no_slot == 1);
    io->fail_qbuf = true; assert(c.read(nullptr) == V4L2Capture::Read::Fault && c.counters().qbuf_errors == 1);
  }
  {
    auto io = std::make_shared<Fixture>(); V4L2Capture c(io); assert(c.open(config().device) && c.start(config(), 1)); auto f = slot();
    for (auto seq : {0xfffffffeu, 0xffffffffu, 0u, 2u, 2u, 1u}) { io->sequence = seq; assert(c.read(&f) == V4L2Capture::Read::Frame); }
    assert(c.counters().sequence_gaps == 1 && c.counters().sequence_duplicates == 1 && c.counters().sequence_discontinuities == 1);
  }
  {
    auto io = std::make_shared<Fixture>(); io->pixfmt = V4L2_PIX_FMT_YUYV;
    io->size = io->maplen = io->bytes = 640 * 360 * 2; io->stride = 1280;
    for (auto& data : io->memory) data.resize(io->size);
    auto cfg = config(); cfg.fourcc = V4L2_PIX_FMT_YUYV;
    V4L2Capture c(io); assert(c.open(cfg.device) && c.start(cfg, 1)); auto f = slot(io->size);
    assert(c.read(&f) == V4L2Capture::Read::Frame && f.stride == 1280);
    io->bytes = 16; assert(c.read(&f) == V4L2Capture::Read::InvalidPayload && !f.valid);
  }
  {
    auto io = std::make_shared<Fixture>(); io->no_frames = true; V4L2Capture c(io);
    auto worker = std::async(std::launch::async, [&] {
      assert(c.open(config().device) && c.start(config(), 1)); auto f = slot();
      auto result = c.read(&f, 1000); c.close(); return result;
    });
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!io->waiting.load() && std::chrono::steady_clock::now() < until) std::this_thread::yield();
    assert(io->waiting.load()); c.request_stop();
    assert(worker.wait_for(std::chrono::milliseconds(500)) == std::future_status::ready);
    assert(worker.get() == V4L2Capture::Read::Stopped && c.cleanup_complete());
  }
  std::puts("PASS V4L2 syscall fixture: layout/budget/requeue/sequence/partial cleanup and real eventfd wake; no camera opened");
}
