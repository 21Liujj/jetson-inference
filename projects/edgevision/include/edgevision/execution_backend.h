#pragma once
#include "edgevision/frame.h"
#include "edgevision/model_kind.h"
#include "edgevision/task_manager.h"
#include <optional>
#include <string>

namespace edgevision {
enum class ModelState { Unloaded, Loading, Ready, Busy, Error };
enum class ExecutionPhase { Idle, WaitingFrame, InFlight, CompletionReady, Stopped, Maintenance };
struct AgentBackendConfig {
  std::string snapshot_root, vlm_binary, llm_engine_dir, visual_engine_dir, plugin_path;
  uint32_t max_snapshots=16, vlm_idle_restore_ms=10000, vlm_max_requests=32;
};
struct InferenceConfig {
  std::string onnx, labels;
  uint32_t max_frame_age_ms = 250;
  DetectorKind kind = DetectorKind::SsdFruit;
  PreprocessMode preprocess = PreprocessMode::CpuBaseline;
  AgentBackendConfig agent;
};
struct ExecutionJob {
  uint64_t task_id = 0, generation = 0, deadline_mono_ns = 0;
  TaskCommand command = TaskCommand::DetectSnapshot;
  ExecutionInput input;
};
struct ExecutionReply { bool accepted = false; std::string error; };
struct ExecutionCompletion {
  uint64_t task_id = 0, generation = 0, completed_mono_ns = 0;
  bool success = false;
  std::string result, error;
  std::optional<FrameMeta> frame;
};
struct ExecutionSnapshot {
  ModelState model = ModelState::Unloaded;
  ExecutionPhase phase = ExecutionPhase::Idle;
  std::optional<uint64_t> task_id;
  bool accepting = false, physical_busy = false, stopped = false;
  uint64_t submitted_jobs = 0, gpu_submissions = 0, gpu_completions = 0;
  uint64_t gpu_submitted_mono_ns = 0;
  std::optional<uint64_t> held_frame_id;
  SlotState held_slot_state = SlotState::Free;
  std::string error;
  ModelState vlm_model=ModelState::Unloaded;
  uint64_t vlm_requests=0;
  uint64_t detector_unloaded_mono_ns=0,detector_ready_mono_ns=0,model_switches=0;
  uint64_t cuda_context_releases=0;
  std::string active_backend,vlm_error;
};
// Physical execution seam. Logical QUEUED/RUNNING/terminal records live only in
// ControlRouter's TaskManager. Calls are bounded/nonblocking and outside its lock.
// A completion may be collected only after every input lease is physically safe.
class ExecutionBackend {
 public:
  virtual ~ExecutionBackend() = default;
  virtual ExecutionReply initialize(InferenceConfig) = 0;
  virtual bool supports_command(TaskCommand c) const noexcept { return c==TaskCommand::DetectSnapshot; }
  virtual bool automatic_model_switching() const noexcept { return false; }
  virtual ExecutionReply submit(ExecutionJob) = 0;
  virtual ExecutionSnapshot query() const = 0;
  virtual std::optional<ExecutionCompletion> collect() = 0;
  virtual void cancel_pending(uint64_t task_id) noexcept = 0;
  virtual void shutdown() noexcept = 0;
};
} // namespace edgevision
