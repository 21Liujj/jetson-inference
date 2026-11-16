#include "edgevision/preview_cache.h"
#include <cstring>
#include <stdexcept>

namespace edgevision {
PreviewCache::PreviewCache(std::size_t bytes, uint64_t period):capacity_(bytes),period_(period) {
  if (!bytes || bytes>16*1024*1024 || !period) throw std::invalid_argument("PREVIEW_BUDGET");
  latest_.data.resize(bytes);
}
void PreviewCache::offer(const Frame& frame) noexcept {
  if (!frame.bytesused || frame.bytesused>capacity_ || frame.bytesused>frame.data.size()) return;
  if (generation_==frame.session_generation && frame.host_dequeue_mono_ns>=last_at_ &&
      frame.host_dequeue_mono_ns-last_at_<period_) return;
  std::unique_lock<std::mutex> lock(mu_,std::try_to_lock);
  if (!lock.owns_lock()) { ++skipped_busy_; return; }
  static_cast<FrameMeta&>(latest_)=frame;
  std::memcpy(latest_.data.data(),frame.data.data(),frame.bytesused);
  latest_.valid=true; valid_=true; generation_=frame.session_generation;
  last_at_=frame.host_dequeue_mono_ns; ++copied_;
}
bool PreviewCache::read(Frame& out) const {
  std::unique_lock<std::mutex> lock(mu_,std::try_to_lock);
  if (!lock.owns_lock() || !valid_) return false;
  static_cast<FrameMeta&>(out)=latest_;
  out.data.assign(latest_.data.begin(),latest_.data.begin()+latest_.bytesused);
  out.valid=true; return true;
}
}
