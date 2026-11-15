#pragma once
#include "edgevision/control_router.h"
#include "edgevision/v4l2_io.h"
#include <atomic>
#include <memory>
#include <string>
#include <vector>

namespace edgevision {
struct CaptureFormat {
  uint32_t width = 0, height = 0, fourcc = 0, stride = 0, sizeimage = 0;
  uint32_t interval_num = 0, interval_den = 0, driver_buffers = 0;
  uint64_t mapped_bytes = 0, pool_bytes = 0;
};
// Linux single-planar MMAP capture, ported from the personal J11 implementation.
// One worker owns open/start/read/stop/close and all snapshots. request_stop()
// alone may run concurrently, while the worker/device lifetime is still held.
// The caller must join the worker before close/destruction; no cross-thread fd
// close or hidden thread is used to interrupt poll.
class V4L2Capture {
 public:
  enum class Read { Frame, Timeout, Stopped, InvalidPayload, NoSlot, Fault };
  struct Counters {
    uint64_t dequeued = 0, frames = 0, no_slot = 0, timeouts = 0;
    uint64_t invalid_payloads = 0, faults = 0, qbuf_errors = 0;
    uint64_t sequence_gaps = 0, sequence_duplicates = 0, sequence_discontinuities = 0;
  };
  explicit V4L2Capture(std::shared_ptr<V4L2Io> io = {});
  ~V4L2Capture() { close(); }
  V4L2Capture(const V4L2Capture&) = delete;
  V4L2Capture& operator=(const V4L2Capture&) = delete;
  bool open(const std::string& device);
  bool start(const CaptureConfig&, uint64_t generation);
  // Frame storage must already reserve negotiated sizeimage. A null slot still
  // dequeues/requeues and counts a pool drop, so capture does not stall on AI.
  Read read(Frame* slot, int timeout_ms = 200);
  void request_stop() noexcept;
  void stop();
  void close();
  bool is_open() const { return fd_ >= 0; }
  bool streaming() const { return streaming_; }
  bool cleanup_complete() const { return fd_ < 0 && stop_fd_ < 0 && mappings_.empty(); }
  const CaptureFormat& format() const { return format_; }
  Counters counters() const { return counters_; }
  const std::string& last_error() const { return last_error_; }
  int last_errno() const { return last_errno_; }
  const std::string& warning() const { return warning_; }
 private:
  struct Mapping { void* addr = nullptr; std::size_t length = 0; bool mapped = false; };
  int control(unsigned long, void*);
  void account_sequence(uint32_t);
  std::shared_ptr<V4L2Io> io_;
  int fd_ = -1, stop_fd_ = -1;
  std::atomic<bool> stop_requested_{false};
  std::vector<Mapping> mappings_;
  CaptureFormat format_;
  Counters counters_;
  bool streaming_ = false, buffers_allocated_ = false;
  std::optional<uint32_t> last_sequence_;
  uint64_t generation_ = 0, frame_id_ = 0;
  std::string last_error_, warning_;
  int last_errno_ = 0;
};
}  // namespace edgevision
