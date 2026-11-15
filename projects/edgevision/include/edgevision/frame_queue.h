#pragma once
#include "edgevision/bounded_queue.h"
#include "edgevision/frame_lease.h"

namespace edgevision {
// One capture generation. Only READY leases may enter this waiting queue;
// INFLIGHT work stays with its executor and cannot be evicted for freshness.
class FrameQueue {
 public:
  enum class Push { Accepted, ReplacedOldest, Stopped, InvalidState, StaleGeneration };
  explicit FrameQueue(uint64_t generation, std::size_t capacity = 2)
      : generation_(generation), queue_(capacity) {}
  Push push(FrameLease&& frame) {
    if (!frame.valid() || frame.state() != SlotState::Ready) return Push::InvalidState;
    if (frame->session_generation != generation_) return Push::StaleGeneration;
    const auto result = queue_.push_latest(std::move(frame));
    if (result == Queue::PushResult::Stopped) return Push::Stopped;
    return result == Queue::PushResult::ReplacedOldest ? Push::ReplacedOldest : Push::Accepted;
  }
  bool consume(FrameLease& out, int timeout_ms) { return queue_.wait_pop(out, timeout_ms); }
  void close() { queue_.shutdown(Queue::StopMode::Abort); }
  std::size_t depth() const { return queue_.size(); }
  uint64_t replaced_oldest() const { return queue_.replaced_oldest(); }
  auto snapshot() const { return queue_.snapshot(); }
 private:
  using Queue = BoundedQueue<FrameLease>;
  const uint64_t generation_;
  Queue queue_;
};
}  // namespace edgevision
