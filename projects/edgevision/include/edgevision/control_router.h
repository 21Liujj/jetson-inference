#pragma once
#include "edgevision/frame.h"
#include "edgevision/task_manager.h"
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
  bool allow_unstable_device = false;
};
struct ControlRequest {
  std::string request_id;
  uint64_t generation = 0, deadline_mono_ns = 0;
  TaskCommand command = TaskCommand::StartCapture;
  std::optional<CaptureConfig> capture;
};
enum class CaptureState { Idle, Starting, Running, Stopping, Recovering, Error };
enum class ModelState { Unloaded, Loading, Ready, Busy, Error };
struct CaptureEvent {
  enum class Kind { FirstFrame, Fault, Stopped };
  Kind kind = Kind::Fault;
  uint64_t generation = 0;
  bool stream_on = false;
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
  std::optional<uint64_t> active_task;
  std::optional<FrameMeta> first_frame;
  std::size_t queued_tasks = 0;
  bool backend_cleanup_pending = false;
  bool backend_resources_live = false;
  std::string last_error;
};

// Transport threads only submit/query/cancel. Exactly one control owner calls
// poll(); all capture-state writes occur there. Backend calls never hold mu_.
// The backend outlives the router and owns RAII cleanup. The application drains
// a stop before destroying the router; destroying this reference is not a stop.
class ControlRouter {
 public:
  explicit ControlRouter(CaptureBackend& backend) : backend_(backend) {}
  TaskAdmission submit(ControlRequest, uint64_t now_mono_ns);
  RouterStatus status() const;
  std::optional<TaskRecord> query(uint64_t task_id) const { return tasks_.query(task_id); }
  bool cancel(uint64_t task_id, uint64_t now_mono_ns);
  void poll(uint64_t now_mono_ns);
  static bool valid_config(const CaptureConfig&);

 private:
  struct Action { enum class Kind { Start, Stop }; Kind kind; uint64_t generation, task; CaptureConfig config; };
  static std::string canonical(const CaptureConfig&);
  static bool terminal(TaskState);
  std::optional<Action> dispatch_locked(const TaskRecord&, uint64_t now);
  std::optional<Action> stop_locked(std::string error);
  std::optional<Action> handle_event_locked(CaptureEvent, uint64_t now);
  void execute(std::optional<Action>, uint64_t now);
  void finish_locked(bool success, const std::string& error, uint64_t now);
  CaptureBackend& backend_;
  mutable std::mutex mu_;
  TaskManager tasks_;
  RouterStatus state_;
  std::optional<std::thread::id> owner_;
  bool backend_live_ = false, error_after_cleanup_ = false;
  std::optional<uint64_t> pending_stop_;
  // One queued and one active command: configuration storage is bounded too.
  std::unordered_map<uint64_t, CaptureConfig> configs_;
};
}  // namespace edgevision
