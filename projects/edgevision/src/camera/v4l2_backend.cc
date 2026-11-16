#include "edgevision/v4l2_backend.h"
#include <chrono>
#include <cerrno>
#include <utility>

namespace edgevision {
namespace {
uint64_t mono_ns() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
CaptureEvent event(CaptureEvent::Kind kind, uint64_t generation, std::string error = {}) {
  CaptureEvent e; e.kind = kind; e.generation = generation; e.error_code = std::move(error); return e;
}
}
V4L2Backend::V4L2Backend(std::shared_ptr<V4L2Io> io) : capture_(std::move(io)), worker_(&V4L2Backend::worker_loop, this) {}
V4L2Backend::~V4L2Backend() {
  {
    std::lock_guard<std::mutex> lock(mu_);
    shutdown_ = true; stop_.store(true);
    if (signal_ready_) capture_.request_stop();
  }
  wake_.notify_one();
  if (worker_.joinable()) worker_.join();
  // Leases own shared storage. Even an application that failed to drain stop
  // cannot make this destructor free an outstanding consumer/GPU buffer early.
}
BackendStartReply V4L2Backend::request_start(const CaptureConfig& config, uint64_t generation) {
  if (!generation || !ControlRouter::valid_config(config)) return {false, "INVALID_CAPTURE_CONFIG"};
  std::lock_guard<std::mutex> lock(mu_);
  if (shutdown_ || pending_ || snapshot_.worker_active || !events_.empty()) return {false, "BACKEND_BUSY"};
  snapshot_ = {}; snapshot_.generation = generation; snapshot_.worker_active = true;
  snapshot_.queue_capacity = config.queue_capacity; snapshot_.pool_capacity = config.pool_slots;
  stop_.store(false); pending_ = Start{config, generation}; wake_.notify_one(); return {true, ""};
}
void V4L2Backend::request_stop(uint64_t generation) noexcept {
  std::lock_guard<std::mutex> lock(mu_);
  if (generation != snapshot_.generation || !snapshot_.worker_active) return;
  stop_.store(true); snapshot_.cleanup_pending = true;
  // Published only while the worker holds a stable eventfd lifetime. The
  // worker clears signal_ready_ under this mutex before closing the channel.
  if (signal_ready_) capture_.request_stop();
  wake_.notify_one();
}
std::optional<CaptureEvent> V4L2Backend::poll_event() {
  std::lock_guard<std::mutex> lock(mu_);
  if (events_.empty()) return std::nullopt;
  auto e = std::move(events_.front()); events_.pop_front(); return e;
}
bool V4L2Backend::consume(FrameLease& lease, int ms) {
  std::shared_ptr<FrameQueue> queue;
  { std::lock_guard<std::mutex> lock(mu_); queue = queue_; }
  return queue && queue->consume(lease, ms);
}
CaptureSnapshot V4L2Backend::snapshot() const {
  std::lock_guard<std::mutex> lock(mu_);
  auto s = snapshot_;
  if (queue_) {
    const auto q = queue_->snapshot();
    s.queue_depth = q.depth; s.queue_high_water = q.high_water; s.queue_replaced = q.replaced_oldest;
    s.queue_enqueued = q.enqueued; s.queue_consumed = q.dequeued; s.queue_aborted = q.aborted;
  }
  if (pool_) s.pool_busy = pool_->busy();
  return s;
}
CaptureHealthSample V4L2Backend::health_sample(uint64_t at) const {
  const auto s = snapshot(); CaptureHealthSample sample;
  sample.generation = s.generation; sample.observed_mono_ns = at;
  auto& c = sample.counters;
  c.valid_frames = s.io.frames + s.io.no_slot; c.pool_drops = s.io.no_slot;
  c.ready_drops = s.queue_replaced; c.stop_drops = s.queue_aborted;
  c.invalid_payloads = s.io.invalid_payloads; c.sequence_gaps = s.io.sequence_gaps; c.timeouts = s.io.timeouts;
  c.queue_depth = s.queue_depth; c.queue_capacity = s.queue_capacity;
  c.pool_busy = s.pool_busy; c.pool_capacity = s.pool_capacity;
  return sample;
}
void V4L2Backend::emit(CaptureEvent e) {
  std::lock_guard<std::mutex> lock(mu_);
  if (e.kind == CaptureEvent::Kind::Fault) snapshot_.error = e.error_code;
  // A session produces FirstFrame, up to four distinct fault notifications,
  // and Stopped. The fixed event budget also covers cleanup retry failures.
  // New sessions are refused until the previous event queue has been consumed.
  if (events_.size() >= 8) { snapshot_.error = "CONTROL_EVENT_BUDGET"; return; }
  events_.push_back(std::move(e));
}
void V4L2Backend::worker_loop() {
  for (;;) {
    Start start;
    {
      std::unique_lock<std::mutex> lock(mu_);
      wake_.wait(lock, [&] { return shutdown_ || pending_.has_value(); });
      if (shutdown_) return;
      start = std::move(*pending_); pending_.reset();
    }
    run(std::move(start));
  }
}
void V4L2Backend::run(Start start) {
  std::shared_ptr<FramePool> pool;
  std::shared_ptr<FrameQueue> queue;
  std::string failure;
  bool startup_retryable = false;
  bool runtime_retryable = false;
  const auto device_error = [](int cause) {
    return cause == EIO || cause == ETIMEDOUT || cause == EOVERFLOW || cause == ENODEV ||
        cause == ENOENT || cause == ENXIO || cause == EPIPE;
  };
  try {
    if (!stop_.load()) {
      if (!capture_.open(start.config.device) || !capture_.start(start.config, start.generation)) {
        failure = capture_.last_error();
        const auto cause = capture_.last_errno();
        startup_retryable = cause == EIO || cause == ETIMEDOUT || cause == EOVERFLOW;
        runtime_retryable = device_error(cause);
      }
      else {
        pool = std::make_shared<FramePool>(start.config.pool_slots, capture_.format().sizeimage);
        queue = std::make_shared<FrameQueue>(start.generation, start.config.queue_capacity);
        std::lock_guard<std::mutex> lock(mu_);
        pool_ = pool; queue_ = queue; signal_ready_ = true;
        snapshot_.format = capture_.format(); snapshot_.warning = capture_.warning();
        if (stop_.load()) capture_.request_stop();
      }
    }
    bool first = true;
    auto last_valid = mono_ns();
    while (failure.empty() && queue && !stop_.load()) {
      const int index = pool->acquire_index();
      FrameLease lease(pool.get(), index); // guards WRITING too, including exceptional exits
      Frame* slot = index >= 0 ? &pool->slot_ref(index) : nullptr;
      const auto result = capture_.read(slot);
      if (result == V4L2Capture::Read::Frame) {
        if (!lease.mark_ready(mono_ns())) { failure = "FRAME_STATE_TRANSITION"; break; }
        const auto meta = static_cast<const FrameMeta&>(*lease);
        const auto pushed = queue->push(std::move(lease));
        if (pushed != FrameQueue::Push::Accepted && pushed != FrameQueue::Push::ReplacedOldest) {
          failure = "FRAME_QUEUE_REJECTED"; break;
        }
        last_valid = mono_ns();
        if (first) {
          auto e = event(CaptureEvent::Kind::FirstFrame, start.generation); e.stream_on = true; e.frame = meta;
          emit(std::move(e)); first = false;
        }
      } else {
        if (result == V4L2Capture::Read::Stopped) break;
        if (result == V4L2Capture::Read::Fault) {
          failure = capture_.last_error(); runtime_retryable = device_error(capture_.last_errno()); break;
        }
        if (result == V4L2Capture::Read::NoSlot) last_valid = mono_ns(); // camera still delivers valid buffers
        if (mono_ns() - last_valid > 3000000000ULL) { failure = "NO_VALID_FRAME_TIMEOUT"; runtime_retryable = true; break; }
      }
      std::lock_guard<std::mutex> lock(mu_); snapshot_.io = capture_.counters();
    }
  } catch (const std::exception& e) { failure = std::string("CAPTURE_EXCEPTION: ") + e.what(); }
  catch (...) { failure = "CAPTURE_EXCEPTION"; }
  {
    std::lock_guard<std::mutex> lock(mu_);
    signal_ready_ = false; snapshot_.cleanup_pending = true; snapshot_.io = capture_.counters();
    snapshot_.error = failure.substr(0, 128);
  }
  if (!failure.empty()) {
    auto fault = event(CaptureEvent::Kind::Fault, start.generation, failure.substr(0, 128));
    fault.retryable = startup_retryable; fault.runtime_retryable = runtime_retryable; emit(std::move(fault));
  }
  if (queue) queue->close(); // stop publication and release queued READY slots
  const auto drain_start = mono_ns(); bool drain_reported = false;
  while (pool && pool->busy()) {
    if (!drain_reported && mono_ns() - drain_start > 5000000000ULL) {
      emit(event(CaptureEvent::Kind::Fault, start.generation, "FRAME_LEASE_DRAIN_TIMEOUT")); drain_reported = true;
    }
    std::unique_lock<std::mutex> lock(mu_);
    if (shutdown_) { lock.unlock(); capture_.close(); return; } // leases retain independent copied storage
    wake_.wait_for(lock, std::chrono::milliseconds(5));
    lock.unlock();
  }
  capture_.close(); // drain owned slots before STREAMOFF/munmap/close
  if (failure.empty() && !capture_.last_error().empty()) {
    failure = "CLEANUP: " + capture_.last_error();
    emit(event(CaptureEvent::Kind::Fault, start.generation, failure.substr(0, 128)));
  }
  if (!capture_.cleanup_complete()) emit(event(CaptureEvent::Kind::Fault, start.generation, "DEVICE_CLEANUP_PENDING"));
  while (!capture_.cleanup_complete()) {
    std::unique_lock<std::mutex> lock(mu_);
    if (shutdown_) return;
    wake_.wait_for(lock, std::chrono::milliseconds(5)); lock.unlock();
    capture_.close();
  }
  {
    std::lock_guard<std::mutex> lock(mu_);
    if (queue) {
      const auto q = queue->snapshot();
      snapshot_.queue_replaced = q.replaced_oldest; snapshot_.queue_high_water = q.high_water;
      snapshot_.queue_enqueued = q.enqueued; snapshot_.queue_consumed = q.dequeued; snapshot_.queue_aborted = q.aborted;
    }
    snapshot_.queue_depth = 0; snapshot_.pool_busy = 0;
    pool_.reset(); queue_.reset(); snapshot_.worker_active = false; snapshot_.cleanup_pending = false;
    if (!failure.empty()) snapshot_.error = failure.substr(0, 128);
    events_.push_back(event(CaptureEvent::Kind::Stopped, start.generation));
  }
}
}  // namespace edgevision
