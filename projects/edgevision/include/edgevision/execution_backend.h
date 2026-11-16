#pragma once
#include "edgevision/frame.h"
#include "edgevision/model_kind.h"
#include <optional>
#include <string>

namespace edgevision {
enum class ModelState { Unloaded, Loading, Ready, Busy, Error };
enum class ExecutionPhase { Idle, WaitingFrame, InFlight, CompletionReady, Stopped };
struct InferenceConfig {
  std::string onnx, labels;
  uint32_t max_frame_age_ms = 250;
  DetectorKind kind = DetectorKind::SsdFruit;
};
struct ExecutionJob {
  uint64_t task_id = 0, generation = 0, deadline_mono_ns = 0;
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
};
// Physical execution seam. Logical QUEUED/RUNNING/terminal records live only in
// ControlRouter's TaskManager. Calls are bounded/nonblocking and outside its lock.
// A completion may be collected only after every input lease is physically safe.
class ExecutionBackend {
 public:
  virtual ~ExecutionBackend() = default;
  virtual ExecutionReply initialize(InferenceConfig) = 0;
  virtual ExecutionReply submit(ExecutionJob) = 0;
  virtual ExecutionSnapshot query() const = 0;
  virtual std::optional<ExecutionCompletion> collect() = 0;
  virtual void cancel_pending(uint64_t task_id) noexcept = 0;
  virtual void shutdown() noexcept = 0;
};
} // namespace edgevision
