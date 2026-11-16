#pragma once
#include "edgevision/frame.h"
#include <atomic>

namespace edgevision {
// Independent, bounded copied preview. The capture producer never waits for a
// reader and this cache never retains a core FrameLease or driver buffer.
class PreviewCache {
 public:
  explicit PreviewCache(std::size_t bytes = 4*1024*1024, uint64_t period_ns = 200000000);
  void offer(const Frame&) noexcept; // single capture producer, try-lock only
  bool read(Frame&) const;          // independent owned copy; may skip when busy
  uint64_t copied() const { return copied_.load(); }
  uint64_t skipped_busy() const { return skipped_busy_.load(); }
 private:
  mutable std::mutex mu_;
  Frame latest_;
  const std::size_t capacity_;
  const uint64_t period_;
  uint64_t last_at_ = 0, generation_ = 0;
  bool valid_ = false;
  std::atomic<uint64_t> copied_{0}, skipped_busy_{0};
};
}
