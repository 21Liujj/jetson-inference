#pragma once
#include <utility>
#include <stdexcept>
#include "edgevision/frame.h"

namespace edgevision {

// A real backend implements this with a completion primitive such as a CUDA
// event. wait() must establish physical completion, not logical expiration.
// It is called without queue/pool locks. This core layer does not fake GPU work.
class CompletionFence {
 public:
  virtual ~CompletionFence() = default;
  virtual bool ready() const noexcept = 0;
  virtual void wait() noexcept = 0;
};

class FrameLease {
 public:
  FrameLease() = default;

  FrameLease(FramePool* pool, int slot)
      : state_(pool?pool->state_:nullptr), slot_(slot), epoch_(FramePool::lease_epoch(state_,slot)) {}

  ~FrameLease() { release(); }

  FrameLease(FrameLease&& other) noexcept
      : state_(std::move(other.state_)), fence_(std::move(other.fence_)), slot_(other.slot_), epoch_(other.epoch_) {
    other.slot_ = -1;
    other.epoch_ = 0;
  }

  FrameLease& operator=(FrameLease&& other) noexcept {
    if (this != &other) {
      release();
      state_ = std::move(other.state_);
      fence_ = std::move(other.fence_);
      slot_ = other.slot_;
      epoch_ = other.epoch_;
      other.slot_ = -1;
      other.epoch_ = 0;
    }
    return *this;
  }

  FrameLease(const FrameLease&) = delete;
  FrameLease& operator=(const FrameLease&) = delete;

  bool valid() const {
    return FramePool::lease_frame(state_,slot_,epoch_)!=nullptr;
  }

  Frame* operator->() { return FramePool::lease_frame(state_,slot_,epoch_); }
  Frame& operator*() { auto* f=operator->();if(!f)throw std::logic_error("invalid FrameLease");return *f; }
  const Frame* operator->() const { return FramePool::lease_frame(state_,slot_,epoch_); }
  const Frame& operator*() const { auto* f=operator->();if(!f)throw std::logic_error("invalid FrameLease");return *f; }

  void release() {
    if (fence_) {
      fence_->wait();
      FramePool::transition(state_,slot_,epoch_,SlotState::InFlight,SlotState::Done);
      fence_.reset();
    }
    FramePool::release_lease(state_,slot_,epoch_);
    state_.reset();slot_=-1;epoch_=0;
  }

  int slot() const { return slot_; }
  SlotState state() const { return FramePool::lease_state(state_,slot_,epoch_); }
  bool mark_ready(uint64_t publish_mono_ns) {
    auto* frame=operator->();
    if(!frame||!FramePool::transition(state_,slot_,epoch_,SlotState::Writing,SlotState::Ready))return false;
    frame->publish_mono_ns=publish_mono_ns;
    return true;
  }
  bool mark_inflight(std::shared_ptr<CompletionFence> fence) {
    if(!fence||!FramePool::transition(state_,slot_,epoch_,SlotState::Ready,SlotState::InFlight))return false;
    fence_=std::move(fence);return true;
  }
  bool complete_if_ready() {
    if(!fence_||!fence_->ready())return false;
    if(!FramePool::transition(state_,slot_,epoch_,SlotState::InFlight,SlotState::Done))return false;
    fence_.reset();return true;
  }

 private:
  std::shared_ptr<detail::FramePoolState> state_;
  std::shared_ptr<CompletionFence> fence_;
  int slot_ = -1;
  uint64_t epoch_ = 0;
};

}  // namespace edgevision
