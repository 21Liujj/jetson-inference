#include "edgevision/control_router.h"
#include <limits>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace edgevision {
bool ControlRouter::valid_config(const CaptureConfig& c) {
  if (c.device.empty() || c.device.size() > 256 || c.device.find('\0') != std::string::npos ||
      c.device.find("..") != std::string::npos) return false;
  const auto stable_name = [&](const std::string& prefix) {
    return c.device.rfind(prefix, 0) == 0 && c.device.size() > prefix.size() &&
        c.device.find('/', prefix.size()) == std::string::npos;
  };
  const bool stable = stable_name("/dev/v4l/by-id/") || stable_name("/dev/v4l/by-path/");
  const bool node = c.device.rfind("/dev/video", 0) == 0 && c.device.size() > 10 &&
      c.device.find_first_not_of("0123456789", 10) == std::string::npos;
  if (!stable && !(c.allow_unstable_device && node)) return false;
  if (!c.width || !c.height || c.width > 8192 || c.height > 8192 ||
      !c.fps_num || !c.fps_den || c.fps_num > 240 || c.fps_den > 1000) return false;
  if (c.fourcc != make_fourcc('Y','U','Y','V') && c.fourcc != make_fourcc('M','J','P','G')) return false;
  if (c.fourcc == make_fourcc('Y','U','Y','V') && c.width % 2) return false;
  if (c.driver_buffers < 2 || c.driver_buffers > 32 || c.pool_slots < 3 || c.pool_slots > 64 ||
      !c.queue_capacity || c.queue_capacity > c.pool_slots - 2 || !c.memory_budget_bytes ||
      c.memory_budget_bytes > 256u * 1024 * 1024) return false;
  const uint64_t conservative_bytes = uint64_t(c.width) * c.height * 2 * (c.driver_buffers + c.pool_slots);
  return conservative_bytes <= c.memory_budget_bytes;
}
std::string ControlRouter::canonical(const CaptureConfig& c) {
  std::ostringstream s;
  s << "capture-v1:" << c.device.size() << ':' << c.device << ':' << c.width << ':' << c.height << ':' << c.fourcc
    << ':' << c.fps_num << ':' << c.fps_den << ':' << c.driver_buffers << ':' << c.pool_slots << ':'
    << c.queue_capacity << ':' << c.memory_budget_bytes << ':' << c.allow_unstable_device;
  return s.str();
}
bool ControlRouter::terminal(TaskState state) {
  return state == TaskState::Done || state == TaskState::Failed || state == TaskState::Canceled || state == TaskState::Expired;
}
TaskAdmission ControlRouter::submit(ControlRequest request, uint64_t now) {
  if (request.command != TaskCommand::StartCapture && request.command != TaskCommand::StopCapture)
    return {false, false, 0, AdmissionError::UnsupportedCommand};
  if ((request.command == TaskCommand::StartCapture && (!request.capture || !valid_config(*request.capture))) ||
      (request.command == TaskCommand::StopCapture && request.capture))
    return {false, false, 0, AdmissionError::InvalidParameters};
  TaskRequest task{request.request_id, request.generation, request.command, request.deadline_mono_ns,
                   request.capture ? canonical(*request.capture) : "stop-v1"};
  std::lock_guard<std::mutex> lock(mu_);
  auto result = tasks_.submit(std::move(task), now, state_.generation);
  if (result.accepted && !result.duplicate && request.capture) configs_.emplace(result.task_id, std::move(*request.capture));
  if (result.accepted && !result.duplicate && request.command == TaskCommand::StopCapture) pending_stop_ = result.task_id;
  return result;
}
bool ControlRouter::cancel(uint64_t id, uint64_t now) {
  // Serialize cancellation with first-frame publication: whichever obtains
  // this lock first wins. A canceled start must never leave capture RUNNING.
  std::lock_guard<std::mutex> lock(mu_);
  return tasks_.cancel(id, now);
}
RouterStatus ControlRouter::status() const {
  std::lock_guard<std::mutex> lock(mu_);
  auto snapshot = state_;
  snapshot.queued_tasks = tasks_.queued();
  snapshot.backend_resources_live = backend_live_;
  snapshot.backend_cleanup_pending = backend_live_ && state_.capture == CaptureState::Stopping;
  return snapshot;
}
void ControlRouter::finish_locked(bool success, const std::string& error, uint64_t now) {
  if (state_.active_task) {
    const auto result = "{\"generation\":" + std::to_string(state_.generation) +
        (state_.first_frame ? ",\"frame_id\":" + std::to_string(state_.first_frame->frame_id) : "") + "}";
    tasks_.complete(*state_.active_task, success, success ? result : "", error, now);
    configs_.erase(*state_.active_task);
    state_.active_task.reset();
  }
}
std::optional<ControlRouter::Action> ControlRouter::dispatch_locked(const TaskRecord& task, uint64_t now) {
  state_.active_task = task.id;
  if (task.request.command == TaskCommand::StartCapture) {
    if (backend_live_ || (state_.capture != CaptureState::Idle && state_.capture != CaptureState::Error)) {
      finish_locked(false, "STATE_CONFLICT", now); return std::nullopt;
    }
    if (state_.generation == std::numeric_limits<uint64_t>::max()) {
      finish_locked(false, "GENERATION_EXHAUSTED", now); return std::nullopt;
    }
    const auto before = state_.generation;
    const auto next = before + 1;
    if (!tasks_.retarget_running(task.id, before, next)) {
      finish_locked(false, "TASK_TRANSITION_FAILED", now); return std::nullopt;
    }
    state_.generation = next;
    state_.capture = CaptureState::Starting;
    state_.first_frame.reset(); state_.last_error.clear(); error_after_cleanup_ = false;
    backend_live_ = true;
    return Action{Action::Kind::Start, next, task.id, configs_.at(task.id)};
  }
  if (!backend_live_) {
    state_.capture = CaptureState::Idle; state_.last_error.clear(); state_.first_frame.reset();
    finish_locked(true, "", now); return std::nullopt;
  }
  if (state_.capture != CaptureState::Stopping) error_after_cleanup_ = false;
  state_.capture = CaptureState::Stopping;
  return Action{Action::Kind::Stop, state_.generation, task.id, {}};
}
std::optional<ControlRouter::Action> ControlRouter::stop_locked(std::string error) {
  if (!error.empty()) { state_.last_error = error.substr(0, 128); error_after_cleanup_ = true; }
  if (!backend_live_ || state_.capture == CaptureState::Stopping) return std::nullopt;
  state_.capture = CaptureState::Stopping;
  return Action{Action::Kind::Stop, state_.generation, state_.active_task.value_or(0), {}};
}
std::optional<ControlRouter::Action> ControlRouter::handle_event_locked(CaptureEvent event, uint64_t now) {
  if (event.generation != state_.generation) { ++state_.stale_events; return std::nullopt; }
  if (!backend_live_) return std::nullopt;
  if (event.kind == CaptureEvent::Kind::Fault) return stop_locked(event.error_code.empty() ? "CAPTURE_FAILED" : event.error_code);
  if (event.kind == CaptureEvent::Kind::Stopped) {
    if (!backend_live_) return std::nullopt;
    if (state_.capture != CaptureState::Stopping) { error_after_cleanup_ = true; state_.last_error = "UNEXPECTED_STOP"; }
    backend_live_ = false;
    state_.capture = error_after_cleanup_ ? CaptureState::Error : CaptureState::Idle;
    state_.first_frame.reset();
    finish_locked(!error_after_cleanup_, state_.last_error, now);
    return std::nullopt;
  }
  if (state_.capture != CaptureState::Starting || !state_.active_task) return std::nullopt;
  auto task = tasks_.query(*state_.active_task);
  if (!task || terminal(task->state)) return stop_locked("");
  const auto& frame = event.frame;
  const auto& config = configs_.at(*state_.active_task);
  bool valid = event.stream_on && frame.session_generation == state_.generation && frame.width && frame.height &&
      frame.width <= 8192 && frame.height <= 8192 && frame.bytesused && frame.host_dequeue_mono_ns &&
      frame.pixfmt_fourcc == config.fourcc && frame.bytesused <= config.memory_budget_bytes;
  if (frame.pixfmt_fourcc == make_fourcc('Y','U','Y','V'))
    valid = valid && frame.width % 2 == 0 && uint64_t(frame.stride) >= uint64_t(frame.width) * 2 &&
        frame.bytesused >= (uint64_t(frame.height) - 1) * frame.stride + uint64_t(frame.width) * 2;
  if (!valid) return stop_locked("INVALID_FIRST_FRAME");
  state_.first_frame = frame;
  state_.capture = CaptureState::Running;
  finish_locked(true, "", now);
  return std::nullopt;
}
void ControlRouter::execute(std::optional<Action> action, uint64_t now) {
  if (!action) return;
  if (action->kind == Action::Kind::Stop) { backend_.request_stop(action->generation); return; }
  BackendStartReply reply;
  try { reply = backend_.request_start(action->config, action->generation); }
  catch (...) {
    // An exception may follow partial resource acquisition. Only a Stopped
    // event may release this operation; do not assume it was a clean rejection.
    std::optional<Action> cleanup;
    { std::lock_guard<std::mutex> lock(mu_); cleanup = stop_locked("BACKEND_START_EXCEPTION"); }
    execute(std::move(cleanup), now);
    return;
  }
  if (!reply.accepted) {
    std::lock_guard<std::mutex> lock(mu_);
    backend_live_ = false; state_.capture = CaptureState::Error;
    state_.last_error = reply.error_code.empty() ? "START_REJECTED" : reply.error_code.substr(0, 128);
    finish_locked(false, state_.last_error, now);
  }
}
void ControlRouter::poll(uint64_t now) {
  std::optional<Action> action;
  {
    std::lock_guard<std::mutex> lock(mu_);
    if (!owner_) owner_ = std::this_thread::get_id();
    if (*owner_ != std::this_thread::get_id()) throw std::logic_error("ControlRouter requires one control owner");
    tasks_.tick(now, state_.generation);
    if (pending_stop_) {
      const auto stop = tasks_.query(*pending_stop_);
      if (!stop || stop->state != TaskState::Queued) pending_stop_.reset();
      else if (state_.capture == CaptureState::Starting && state_.active_task) {
        // A user stop must wake a no-frame start immediately. The queued stop
        // still completes only after physical cleanup, through TaskManager.
        tasks_.cancel(*state_.active_task, now);
        action = stop_locked("");
      }
    }
    // Expired/canceled queued configurations must not outlive their records.
    for (auto it = configs_.begin(); it != configs_.end();) {
      const auto record = tasks_.query(it->first);
      if (!record || (terminal(record->state) && !record->execution_pending)) it = configs_.erase(it); else ++it;
    }
    if (state_.active_task) {
      auto record = tasks_.query(*state_.active_task);
      if (record && terminal(record->state) && state_.capture == CaptureState::Starting) action = stop_locked("");
    }
  }
  execute(std::move(action), now);
  action.reset();
  // Bound event processing so a faulty backend cannot starve command handling.
  for (unsigned i = 0; i < 16; ++i) {
    std::optional<CaptureEvent> event;
    try { event = backend_.poll_event(); }
    catch (...) {
      std::lock_guard<std::mutex> lock(mu_);
      action = stop_locked("BACKEND_EVENT_EXCEPTION");
    }
    if (action) { execute(std::move(action), now); action.reset(); break; }
    if (!event) break;
    { std::lock_guard<std::mutex> lock(mu_); action = handle_event_locked(std::move(*event), now); }
    execute(std::move(action), now); action.reset();
  }
  {
    std::lock_guard<std::mutex> lock(mu_);
    if (!state_.active_task && !tasks_.execution_busy()) {
      auto next = tasks_.take(now, state_.generation);
      if (next) action = dispatch_locked(*next, now);
    }
  }
  execute(std::move(action), now);
}
}  // namespace edgevision
