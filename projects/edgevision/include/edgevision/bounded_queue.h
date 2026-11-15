#pragma once
// BoundedQueue<T>: bounded FIFO; reject-newest or replace-oldest admission.
//
// Why bounded (never unbounded): on an 8GB edge box, an unbounded queue
// turns a transient consumer stall (e.g. inference jitter) into OOM.
// Bounded + explicit drop counter makes backpressure VISIBLE in metrics.
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <queue>
#include <stdexcept>
#include <utility>

namespace edgevision {

template <typename T>
class BoundedQueue {
 public:
  enum class StopMode { Drain, Abort };
  struct Snapshot {
    uint64_t enqueued,dequeued,dropped_full,rejected_stopped,aborted;
    std::size_t depth,high_water;
    bool stopped;
  };
  enum class PushResult { Enqueued, ReplacedOldest, Stopped };
  explicit BoundedQueue(std::size_t capacity) : capacity_(capacity) {
    if (!capacity) throw std::invalid_argument("queue capacity must be positive");
  }
  ~BoundedQueue() = default;
  BoundedQueue(const BoundedQueue&) = delete;
  BoundedQueue& operator=(const BoundedQueue&) = delete;

  // No capacity wait. Rejected move-only resources are destroyed after unlock.
  bool try_push(T item) {
    {
      std::lock_guard<std::mutex> lk(mu_);
      if(shutdown_){++rejected_stopped_;return false;}
      if(q_.size()>=capacity_){++dropped_;return false;}
      q_.push(std::move(item));++enqueued_;
      if(q_.size()>high_water_)high_water_=q_.size();
    }
    cv_.notify_one();
    return true;
  }

  // Freshness policy for READY frames only. The removed item's destructor
  // runs after unlocking; in-flight work must never be stored in this queue.
  PushResult push_latest(T item) {
    std::optional<T> removed;
    bool replaced = false;
    {
      std::lock_guard<std::mutex> lock(mu_);
      if (shutdown_) { ++rejected_stopped_; return PushResult::Stopped; }
      if (q_.size() == capacity_) {
        removed.emplace(std::move(q_.front()));
        q_.pop();
        ++replaced_oldest_;
        replaced = true;
      }
      q_.push(std::move(item));
      ++enqueued_;
      if (q_.size() > high_water_) high_water_ = q_.size();
    }
    cv_.notify_one();
    return replaced ? PushResult::ReplacedOldest : PushResult::Enqueued;
  }
  uint64_t replaced_oldest() const {
    std::lock_guard<std::mutex> lock(mu_);
    return replaced_oldest_;
  }

  // timeout<0 waits indefinitely, stop always wakes it. Move operations must
  // be cheap/nonblocking. Release of the caller's previous out is outside mu.
  bool wait_pop(T& out, int timeout_ms) {
    std::optional<T> item;
    {
      std::unique_lock<std::mutex> lk(mu_);
      const auto ready=[&]{return !q_.empty()||shutdown_;};
      if(timeout_ms<0)cv_.wait(lk,ready);
      else if(!cv_.wait_for(lk,std::chrono::milliseconds(timeout_ms),ready))return false;
      if(q_.empty())return false;
      item.emplace(std::move(q_.front()));q_.pop();++dequeued_;
    }
    out=std::move(*item);
    return true;
  }

  // Drain preserves accepted FIFO items; Abort releases pending items outside
  // the mutex. Drain may be upgraded to Abort. No reopen; create a new queue.
  // Owner MUST wake/join every user before destroying this queue.
  void shutdown(StopMode mode=StopMode::Drain) {
    std::queue<T> removed;
    {
      std::lock_guard<std::mutex> lk(mu_);shutdown_=true;
      if(mode==StopMode::Abort){aborted_+=q_.size();q_.swap(removed);}
    }
    cv_.notify_all();
  }
  Snapshot snapshot() const {
    std::lock_guard<std::mutex> lk(mu_);
    return {enqueued_,dequeued_,dropped_,rejected_stopped_,aborted_,q_.size(),high_water_,shutdown_};
  }

  std::size_t size() const {
    std::lock_guard<std::mutex> lk(mu_);
    return q_.size();
  }
  uint64_t dropped() const {
    std::lock_guard<std::mutex> lk(mu_);
    return dropped_;
  }

 private:
  const std::size_t capacity_;
  mutable std::mutex mu_;
  std::condition_variable cv_;
  std::queue<T> q_;
  uint64_t dropped_ = 0;
  uint64_t enqueued_=0,dequeued_=0,rejected_stopped_=0,aborted_=0;
  uint64_t replaced_oldest_=0;
  std::size_t high_water_=0;
  bool shutdown_ = false;
};

}  // namespace edgevision
