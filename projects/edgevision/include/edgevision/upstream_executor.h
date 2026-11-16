#pragma once
#include "edgevision/execution_backend.h"
#include "edgevision/v4l2_backend.h"
#include <condition_variable>
#include <functional>

namespace edgevision {
// One bounded job/completion slot; no second logical task manager. The capture
// backend outlives this object. shutdown() requests drain; query().stopped is
// physical acknowledgement, then the destructor can join without GPU work.
class UpstreamExecutor final : public ExecutionBackend {
 public:
  // Optional code-only hook for explicitly labelled GPU acceptance injection.
  // Product control/configuration has no way to set or execute this callback.
  explicit UpstreamExecutor(V4L2Backend&, HealthMonitor* = nullptr, std::function<void()> acceptance_gpu_work = {});
  ~UpstreamExecutor() override;
  ExecutionReply initialize(InferenceConfig) override;
  ExecutionReply submit(ExecutionJob) override;
  ExecutionSnapshot query() const override;
  std::optional<ExecutionCompletion> collect() override;
  void cancel_pending(uint64_t) noexcept override;
  void shutdown() noexcept override;
 private:
  void worker_loop();
  V4L2Backend& capture_;
  HealthMonitor* const health_;
  const std::function<void()> acceptance_gpu_work_;
  mutable std::mutex mu_;
  std::condition_variable wake_;
  std::optional<InferenceConfig> config_;
  std::optional<ExecutionJob> job_;
  std::optional<ExecutionCompletion> completion_;
  ExecutionSnapshot state_;
  bool closing_ = false, cancel_requested_ = false;
  std::thread worker_;
};
} // namespace edgevision
