#pragma once
#include "edgevision/execution_backend.h"
#include <functional>
#include <memory>

namespace edgevision {
struct VlmResponse {
  std::string text, raw_json;
  uint64_t started_mono_ns=0,finished_mono_ns=0;
};
// One owner thread and one request. Child processes own all VLM device memory;
// no core frame lease or CUDA address crosses this JSONL process boundary.
class VlmProcess {
 public:
  explicit VlmProcess(AgentBackendConfig);
  ~VlmProcess();
  VlmProcess(const VlmProcess&)=delete;
  VlmProcess& operator=(const VlmProcess&)=delete;
  VlmResponse generate(const std::string& request_id,const std::string& system_prompt,
      const std::string& prompt,const std::string& image,unsigned max_tokens,
      uint64_t deadline_mono_ns,const std::function<bool()>& canceled,const std::function<void()>& submitted={});
  void stop(bool force=false) noexcept;
  bool alive() const noexcept;
  uint64_t last_activity_mono_ns()const noexcept;
  uint64_t requests() const noexcept;
 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
}
