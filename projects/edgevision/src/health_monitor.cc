#include "edgevision/health_monitor.h"
#include <algorithm>
#include <stdexcept>
#include <utility>

namespace edgevision {
HealthMonitor::HealthMonitor() : HealthMonitor(Limits{}) {}
HealthMonitor::HealthMonitor(Limits limits) : limits_(limits) {
  if (limits.capture_samples < 2 || limits.capture_samples > 1024 || !limits.latency_samples ||
      limits.latency_samples > 8192 || !limits.window_ns || !limits.stall_ns)
    throw std::invalid_argument("invalid health history/window budget");
}
bool HealthMonitor::observe_capture(CaptureHealthSample s) {
  std::lock_guard<std::mutex> lock(mu_);
  const auto& c = s.counters;
  if (!s.generation || !s.observed_mono_ns || s.generation < latest_.generation ||
      s.observed_mono_ns <= latest_.observed_mono_ns || c.queue_depth > c.queue_capacity ||
      c.pool_busy > c.pool_capacity || c.pool_capacity > 1024 || c.queue_capacity > 1024) {
    ++rejected_; return false;
  }
  const bool next = s.generation != latest_.generation;
  const auto& old = latest_.counters;
  if (!next && (c.valid_frames < old.valid_frames || c.pool_drops < old.pool_drops || c.ready_drops < old.ready_drops ||
      c.stop_drops < old.stop_drops || c.invalid_payloads < old.invalid_payloads || c.sequence_gaps < old.sequence_gaps || c.timeouts < old.timeouts)) {
    ++rejected_; return false;
  }
  if (next) { captures_.clear(); consumed_.clear(); results_.clear(); last_frame_.reset(); }
  if (next || c.valid_frames > old.valid_frames) last_progress_ = s.observed_mono_ns;
  latest_ = s; captures_.push_back(std::move(s));
  while (captures_.size() > limits_.capture_samples || latest_.observed_mono_ns - captures_.front().observed_mono_ns > limits_.window_ns)
    captures_.pop_front();
  return true;
}
void HealthMonitor::append(std::deque<TimedValue>& samples, TimedValue value) {
  samples.push_back(value);
  while (samples.size() > limits_.latency_samples || value.at - samples.front().at > limits_.window_ns) samples.pop_front();
}
bool HealthMonitor::observe_frame(const FrameMeta& m, uint64_t at, bool result) {
  std::lock_guard<std::mutex> lock(mu_);
  if (m.session_generation < latest_.generation) { ++stale_frames_; return false; }
  auto& samples = result ? results_ : consumed_;
  if (!latest_.generation || m.session_generation != latest_.generation || !m.host_dequeue_mono_ns ||
      m.publish_mono_ns < m.host_dequeue_mono_ns || at < m.publish_mono_ns ||
      (!samples.empty() && at < samples.back().at)) { ++rejected_; return false; }
  append(samples, {at, at - m.host_dequeue_mono_ns});
  if (!last_frame_ || m.host_dequeue_mono_ns >= last_frame_->host_dequeue_mono_ns) last_frame_ = m;
  return true;
}
bool HealthMonitor::observe_consumed(const FrameMeta& frame, uint64_t at) { return observe_frame(frame, at, false); }
bool HealthMonitor::observe_result(const FrameMeta& frame, uint64_t at) { return observe_frame(frame, at, true); }
bool HealthMonitor::observe_process(ProcessHealthSample sample) {
  std::lock_guard<std::mutex> lock(mu_);
  if (!sample.observed_mono_ns || (process_ && sample.observed_mono_ns <= process_->observed_mono_ns)) { ++rejected_; return false; }
  process_ = std::move(sample); return true;
}
LatencyWindow HealthMonitor::distribution(const std::deque<TimedValue>& samples, uint64_t now, uint64_t window) {
  std::vector<uint64_t> values; values.reserve(samples.size());
  for (auto s : samples) if (s.at <= now && now - s.at <= window) values.push_back(s.value);
  if (values.empty()) return {};
  std::sort(values.begin(), values.end());
  auto percentile = [&](std::size_t p) { return values[(p * values.size() + 99) / 100 - 1]; };
  return {values.size(), percentile(50), percentile(95), percentile(99), values.back()};
}
HealthSnapshot HealthMonitor::snapshot(uint64_t now, const RouterStatus& router) const {
  HealthSnapshot out;
  std::deque<CaptureHealthSample> captures;
  std::deque<TimedValue> consumed, results;
  std::optional<FrameMeta> frame;
  uint64_t progress;
  {
    std::lock_guard<std::mutex> lock(mu_);
    out.generation = latest_.generation; out.observed_mono_ns = latest_.observed_mono_ns;
    out.has_capture_sample = latest_.generation != 0; out.counters = latest_.counters;
    out.rejected_samples = rejected_; out.stale_frame_reports = stale_frames_;
    captures = captures_; consumed = consumed_; results = results_; frame = last_frame_;
    progress = last_progress_;
    if (process_ && process_->observed_mono_ns <= now) out.process = process_;
  }
  out.router = router; out.generation_matches = out.has_capture_sample && out.generation == router.generation;
  out.telemetry_stale = !out.observed_mono_ns || now < out.observed_mono_ns || now - out.observed_mono_ns >= limits_.stall_ns;
  out.observed_capture_stalled = out.generation_matches && router.capture == CaptureState::Running && progress &&
      now >= progress && now - progress >= limits_.stall_ns;
  if (!out.generation_matches) return out;
  const CaptureHealthSample* first = nullptr; const CaptureHealthSample* last = nullptr;
  for (const auto& sample : captures) if (sample.observed_mono_ns <= now && now - sample.observed_mono_ns <= limits_.window_ns) {
    if (!first) first = &sample; last = &sample; ++out.rate.samples;
  }
  if (first && last && last->observed_mono_ns > first->observed_mono_ns) {
    out.rate.interval_ns = last->observed_mono_ns - first->observed_mono_ns;
    out.rate.valid_capture_fps = double(last->counters.valid_frames - first->counters.valid_frames) * 1e9 / out.rate.interval_ns;
  }
  out.dequeue_to_consume = distribution(consumed, now, limits_.window_ns);
  out.dequeue_to_result = distribution(results, now, limits_.window_ns);
  if (frame && frame->host_dequeue_mono_ns <= now) {
    out.last_frame_id = frame->frame_id; out.frame_age_from_dequeue_ns = now - frame->host_dequeue_mono_ns;
  }
  return out;
}
}  // namespace edgevision
