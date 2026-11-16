#pragma once
#include "edgevision/snapshot_store.h"
#include "edgevision/vlm_process.h"

namespace edgevision {
// Snapshot codec and model protocol adapter. This object and its model child
// belong to the same physical executor worker as UpstreamDetector.
class AgentTools {
 public:
  explicit AgentTools(const AgentBackendConfig&);
  SnapshotRecord save(const Frame&,uint64_t task,uint64_t now);
  FrameLease replay(const std::string&,uint64_t generation,uint64_t now)const;
  SnapshotRecord query(const std::string& id,uint64_t generation,uint64_t now)const{return snapshots_.query(id,generation,now);}
  ExecutionCompletion snapshot_result(const ExecutionJob&,const Frame&);
  ExecutionCompletion model_task(const ExecutionJob&,const std::function<bool()>& canceled,const std::function<void()>& submitted={});
  void stop_model(bool force=false)noexcept{if(model_)model_->stop(force);}
  bool model_alive()const noexcept{return model_&&model_->alive();}
  uint64_t model_requests()const noexcept{return model_?model_->requests():0;}
  uint64_t last_activity()const noexcept{return model_?model_->last_activity_mono_ns():0;}
 private:
  SnapshotStore snapshots_;
  std::unique_ptr<VlmProcess> model_;
};
}
