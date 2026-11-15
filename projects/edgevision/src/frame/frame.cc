#include "edgevision/frame.h"
#include <limits>
#include <stdexcept>
namespace edgevision {
namespace detail {
struct FramePoolState {
  FramePoolState(std::size_t count,std::size_t bytes):frames(count),states(count,SlotState::Free),epochs(count,0) {
    for(auto& f:frames){f.data.reserve(bytes);f.valid=false;}
  }
  std::vector<Frame> frames;
  std::vector<SlotState> states;
  std::vector<uint64_t> epochs;
  mutable std::mutex mu;
  uint64_t next_id=0;
  int acquire() {
    for(std::size_t i=0;i<frames.size();++i)if(states[i]==SlotState::Free&&epochs[i]!=UINT64_MAX) {
      states[i]=SlotState::Writing;++epochs[i];auto& f=frames[i];
      static_cast<FrameMeta&>(f)=FrameMeta{};
      f.frame_id=next_id++;f.epoch=epochs[i];f.valid=false;f.data.clear();return static_cast<int>(i);
    }
    return -1;
  }
};
}
FramePool::FramePool(std::size_t count,std::size_t bytes){
  if(!count||count>1024||!bytes||bytes>64u*1024*1024||count>256u*1024*1024/bytes)
    throw std::invalid_argument("frame pool exceeds the 256 MiB/1024-slot budget");
  state_=std::make_shared<detail::FramePoolState>(count,bytes);
}
int FramePool::acquire_index(){std::lock_guard<std::mutex> lock(state_->mu);return state_->acquire();}
Frame& FramePool::slot_ref(int slot){return state_->frames.at(static_cast<std::size_t>(slot));}
int FramePool::acquire(Frame& out){
  std::lock_guard<std::mutex> lock(state_->mu);int slot=state_->acquire();
  if(slot>=0)out=state_->frames[slot];return slot;
}
void FramePool::release(int slot){
  std::lock_guard<std::mutex> lock(state_->mu);
  if(slot>=0&&static_cast<std::size_t>(slot)<state_->frames.size()&&state_->states[slot]!=SlotState::InFlight){
    state_->states[slot]=SlotState::Free;state_->frames[slot].valid=false;
  }
}
uint64_t FramePool::lease_epoch(const std::shared_ptr<detail::FramePoolState>& state,int slot){
  if(!state)return 0;std::lock_guard<std::mutex> lock(state->mu);
  if(slot<0||static_cast<std::size_t>(slot)>=state->epochs.size()||state->states[slot]==SlotState::Free)return 0;
  return state->epochs[slot];
}
uint64_t FramePool::epoch_of(int slot) const {
  std::lock_guard<std::mutex> lock(state_->mu);
  return slot<0||static_cast<std::size_t>(slot)>=state_->epochs.size()?0:state_->epochs[slot];
}
Frame* FramePool::lease_frame(const std::shared_ptr<detail::FramePoolState>& state,int slot,uint64_t epoch){
  if(!state||!epoch)return nullptr;std::lock_guard<std::mutex> lock(state->mu);
  if(slot<0||static_cast<std::size_t>(slot)>=state->frames.size()||state->states[slot]==SlotState::Free||
     state->epochs[slot]!=epoch||!state->frames[slot].valid)return nullptr;
  return &state->frames[slot];
}
void FramePool::release_lease(const std::shared_ptr<detail::FramePoolState>& state,int slot,uint64_t epoch){
  if(!state||!epoch)return;std::lock_guard<std::mutex> lock(state->mu);
  if(slot>=0&&static_cast<std::size_t>(slot)<state->frames.size()&&state->states[slot]!=SlotState::InFlight&&state->epochs[slot]==epoch){
    state->states[slot]=SlotState::Free;state->frames[slot].valid=false;
  }
}
std::size_t FramePool::busy() const {
  std::lock_guard<std::mutex> lock(state_->mu);std::size_t n=0;for(auto s:state_->states)n+=s!=SlotState::Free;return n;
}
std::size_t FramePool::slots() const{return state_->frames.size();}
std::size_t FramePool::slots_in_state(SlotState wanted) const {
  std::lock_guard<std::mutex> lock(state_->mu);std::size_t n=0;for(auto s:state_->states)n+=s==wanted;return n;
}
SlotState FramePool::lease_state(const std::shared_ptr<detail::FramePoolState>& state,int slot,uint64_t epoch){
  if(!state||!epoch)return SlotState::Free;std::lock_guard<std::mutex> lock(state->mu);
  if(slot<0||static_cast<std::size_t>(slot)>=state->states.size()||state->epochs[slot]!=epoch)return SlotState::Free;
  return state->states[slot];
}
bool FramePool::transition(const std::shared_ptr<detail::FramePoolState>& state,int slot,uint64_t epoch,SlotState from,SlotState to){
  if(!state||!epoch)return false;std::lock_guard<std::mutex> lock(state->mu);
  if(slot<0||static_cast<std::size_t>(slot)>=state->states.size()||state->epochs[slot]!=epoch||state->states[slot]!=from)return false;
  state->states[slot]=to;return true;
}
} // namespace edgevision
