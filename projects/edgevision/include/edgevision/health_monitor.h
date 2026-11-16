#pragma once
#include "edgevision/control_router.h"
#include <deque>
#include <vector>

namespace edgevision {
struct CaptureHealthCounters {
  uint64_t valid_frames = 0, pool_drops = 0, ready_drops = 0, stop_drops = 0;
  uint64_t invalid_payloads = 0, sequence_gaps = 0, timeouts = 0;
  std::size_t queue_depth = 0, queue_capacity = 0, pool_busy = 0, pool_capacity = 0;
};
struct CaptureHealthSample {
  uint64_t generation = 0, observed_mono_ns = 0;
  CaptureHealthCounters counters;
};
struct ProcessHealthSample {
  uint64_t observed_mono_ns = 0;
  std::optional<uint64_t> rss_kib, fd_count;
};
struct LatencyWindow {
  std::size_t samples = 0;
  std::optional<uint64_t> p50_ns, p95_ns, p99_ns, max_ns;
};
struct RateWindow {
  std::size_t samples = 0;
  uint64_t interval_ns = 0;
  std::optional<double> valid_capture_fps;
};
struct HealthSnapshot {
  uint64_t generation = 0, observed_mono_ns = 0;
  bool has_capture_sample = false, generation_matches = false;
  bool telemetry_stale = true, observed_capture_stalled = false;
  uint64_t rejected_samples = 0, stale_frame_reports = 0;
  CaptureHealthCounters counters;
  RateWindow rate;
  LatencyWindow dequeue_to_consume, dequeue_to_result;
  std::optional<uint64_t> last_frame_id, frame_age_from_dequeue_ns;
  std::optional<ProcessHealthSample> process;
  RouterStatus router;
};
// Fixed-memory rolling diagnostics, no /proc, logging, device IO or callbacks.
// Capture rates use cumulative valid DQBUF counters (including valid pool drops),
// not inference/preview counts. Stage samples are reported once by their owner.
class HealthMonitor {
 public:
  struct Limits {
    std::size_t capture_samples = 120, latency_samples = 600;
    uint64_t window_ns = 10000000000ULL, stall_ns = 3000000000ULL;
  };
  HealthMonitor();
  explicit HealthMonitor(Limits);
  bool observe_capture(CaptureHealthSample);
  bool observe_consumed(const FrameMeta&, uint64_t observed_mono_ns);
  // CPU-observed result completion, not CUDA kernel duration or enqueue time.
  bool observe_result(const FrameMeta&, uint64_t completed_mono_ns);
  bool observe_process(ProcessHealthSample);
  HealthSnapshot snapshot(uint64_t now_mono_ns, const RouterStatus&) const;
 private:
  struct TimedValue { uint64_t at, value; };
  bool observe_frame(const FrameMeta&, uint64_t, bool result);
  void append(std::deque<TimedValue>&, TimedValue);
  static LatencyWindow distribution(const std::deque<TimedValue>&, uint64_t now, uint64_t window);
  Limits limits_;
  mutable std::mutex mu_;
  CaptureHealthSample latest_;
  std::deque<CaptureHealthSample> captures_;
  std::deque<TimedValue> consumed_, results_;
  std::optional<FrameMeta> last_frame_;
  std::optional<ProcessHealthSample> process_;
  uint64_t last_progress_ = 0, rejected_ = 0, stale_frames_ = 0;
};
}  // namespace edgevision
