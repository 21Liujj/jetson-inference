#pragma once
#include "edgevision/frame.h"
#include "edgevision/task_manager.h"
#include "edgevision/execution_backend.h"
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>

namespace edgevision {
constexpr uint32_t make_fourcc(char a, char b, char c, char d) {
  return uint32_t(a) | (uint32_t(b) << 8) | (uint32_t(c) << 16) | (uint32_t(d) << 24);
}
struct CaptureConfig {
  std::string device;
  uint32_t width = 640, height = 360, fourcc = make_fourcc('Y','U','Y','V');
  uint32_t fps_num = 30, fps_den = 1;
  uint32_t driver_buffers = 4, pool_slots = 4, queue_capacity = 2;
  uint64_t memory_budget_bytes = 64u * 1024 * 1024;
  uint32_t start_retry_limit = 2, retry_delay_ms = 250;
  // Total physical reopen attempts per explicit start, not reset by flapping.
  uint32_t runtime_retry_limit = 3, runtime_retry_delay_ms = 1000;
  uint32_t runtime_recovery_budget_ms = 15000;
  bool allow_unstable_device = false;
};
struct ControlRequest {
  std::string request_id;
  uint64_t generation = 0, deadline_mono_ns = 0;
  TaskCommand command = TaskCommand::StartCapture;
  std::optional<CaptureConfig> capture;
  ExecutionInput input;
};
enum class CaptureState { Idle, Starting, Running, Stopping, Recovering, Error };
struct CaptureEvent {
  enum class Kind { FirstFrame, Fault, Stopped };
  Kind kind = Kind::Fault;
  uint64_t generation = 0;
  bool stream_on = false;
  bool retryable = false; // initial-start transient I/O failures only
  bool runtime_retryable = false; // includes absent device during an established session
  FrameMeta frame;
  std::string error_code;
};
struct BackendStartReply { bool accepted = false; std::string error_code; };

// Mandatory real-backend seam. No default mock exists. Calls must be bounded
// and nonblocking: the backend owns the capture worker and device lifecycle.
// Accepted start must eventually emit FirstFrame/Fault and Stopped on cleanup.
// A rejected start guarantees no live worker/device resources for that request.
class CaptureBackend {
 public:
  virtual ~CaptureBackend() = default;
  virtual BackendStartReply request_start(const CaptureConfig&, uint64_t generation) = 0;
  virtual void request_stop(uint64_t generation) noexcept = 0;
  virtual std::optional<CaptureEvent> poll_event() = 0;
};
struct RouterStatus {
  CaptureState capture = CaptureState::Idle;
  ModelState model = ModelState::Unloaded;
  uint64_t generation = 0, stale_events = 0;
  uint32_t start_retries = 0;
  uint32_t runtime_retries = 0;
  bool runtime_recovery_active = false;
  uint64_t fault_detected_mono_ns = 0, recovered_mono_ns = 0;
  std::optional<uint64_t> active_task;
  std::optional<FrameMeta> first_frame;
  std::size_t queued_tasks = 0;
  bool backend_cleanup_pending = false;
  bool backend_resources_live = false;
  std::string last_error;
  std::string model_error;
  std::optional<uint64_t> inference_task;
  bool shutting_down = false, shutdown_complete = false;
};

// Transport threads only submit/query/cancel. Exactly one control owner calls
// poll(); all capture-state writes occur there. Backend calls never hold mu_.
// The backend outlives the router and owns RAII cleanup. The application drains
// a stop before destroying the router; destroying this reference is not a stop.
class ControlRouter {
 public:
  explicit ControlRouter(CaptureBackend& backend, ExecutionBackend* executor = nullptr, std::size_t retained_tasks = 256)
      : backend_(backend), executor_(executor), tasks_(1,retained_tasks) {}
  ExecutionReply initialize(InferenceConfig);
  TaskAdmission submit(ControlRequest, uint64_t now_mono_ns);
  RouterStatus status() const;
  std::optional<TaskRecord> query(uint64_t task_id) const { return tasks_.query(task_id); }
  bool cancel(uint64_t task_id, uint64_t now_mono_ns);
  std::optional<TaskRecord> collect(uint64_t task_id) const;
  bool release_internal_result(uint64_t task_id) { return tasks_.release_terminal(task_id); }
  void cancel_pending(uint64_t now_mono_ns);
  void shutdown(uint64_t now_mono_ns);
  void poll(uint64_t now_mono_ns);
  // The control owner publishes worker observations; capture is independent.
  void observe_model(ModelState, std::string error = {});
  static bool valid_config(const CaptureConfig&);

 private:
  struct Action { enum class Kind { Start, Stop, Detect }; Kind kind; uint64_t generation, task; CaptureConfig config;
    TaskCommand command=TaskCommand::DetectSnapshot;ExecutionInput input; };
  static std::string canonical(const CaptureConfig&);
  static bool terminal(TaskState);
  std::optional<Action> dispatch_locked(const TaskRecord&, uint64_t now);
  std::optional<Action> stop_locked(std::string error);
  std::optional<Action> handle_event_locked(CaptureEvent, uint64_t now);
  void execute(std::optional<Action>, uint64_t now);
  void finish_locked(bool success, const std::string& error, uint64_t now);
  std::optional<Action> reopen_locked(uint64_t now);
  void poll_executor(uint64_t now);
  void require_owner_locked();
  CaptureBackend& backend_;
  ExecutionBackend* const executor_;
  mutable std::mutex mu_;
  TaskManager tasks_;
  RouterStatus state_;
  std::optional<std::thread::id> owner_;
  bool backend_live_ = false, error_after_cleanup_ = false;
  enum class Recovery { None, Startup, Runtime };
  Recovery recovery_pending_ = Recovery::None;
  std::optional<CaptureConfig> session_config_;
  bool session_requested_ = false;
  uint64_t runtime_deadline_ = 0;
  uint64_t retry_at_ = 0;
  std::optional<uint64_t> pending_stop_;
  std::optional<uint64_t> inference_task_, preempted_stop_;
  bool execution_cancel_sent_ = false, shutting_down_ = false, executor_stopped_ = false;
  bool automatic_model_switching_=false;
  // One queued and one active command: configuration storage is bounded too.
  std::unordered_map<uint64_t, CaptureConfig> configs_;
};
}  // namespace edgevision
