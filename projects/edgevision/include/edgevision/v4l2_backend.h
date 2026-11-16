#pragma once
#include "edgevision/frame_queue.h"
#include "edgevision/v4l2_capture.h"
#include "edgevision/health_monitor.h"
#include <condition_variable>
#include <deque>

namespace edgevision {
struct CaptureSnapshot {
  uint64_t generation = 0, queue_replaced = 0;
  uint64_t queue_enqueued = 0, queue_consumed = 0, queue_aborted = 0;
  std::size_t queue_high_water = 0;
  std::size_t queue_capacity = 0, pool_capacity = 0;
  CaptureFormat format;
  V4L2Capture::Counters io;
  std::size_t queue_depth = 0, pool_busy = 0;
  bool worker_active = false, cleanup_pending = false;
  std::string error, warning;
};
// One persistent capture worker is the only device owner. Control calls enqueue
// commands or signal eventfd; they never open/ioctl/mmap/join the device worker.
// This is the real Linux backend. I/O injection is only for explicit tests.
class V4L2Backend final : public CaptureBackend {
 public:
  explicit V4L2Backend(std::shared_ptr<V4L2Io> io = {});
  ~V4L2Backend() override;
  BackendStartReply request_start(const CaptureConfig&, uint64_t generation) override;
  void request_stop(uint64_t generation) noexcept override;
  std::optional<CaptureEvent> poll_event() override;
  bool consume(FrameLease&, int timeout_ms);
  CaptureSnapshot snapshot() const;
  CaptureHealthSample health_sample(uint64_t observed_mono_ns) const;
 private:
  struct Start { CaptureConfig config; uint64_t generation; };
  void worker_loop();
  void run(Start);
  void emit(CaptureEvent);
  mutable std::mutex mu_;
  std::condition_variable wake_;
  V4L2Capture capture_;
  std::optional<Start> pending_;
  std::shared_ptr<FramePool> pool_;
  std::shared_ptr<FrameQueue> queue_;
  std::deque<CaptureEvent> events_;
  CaptureSnapshot snapshot_;
  std::atomic<bool> stop_{false};
  bool shutdown_ = false, signal_ready_ = false;
  std::thread worker_;
};
}  // namespace edgevision
