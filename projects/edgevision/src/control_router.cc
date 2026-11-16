#include "edgevision/control_router.h"
#include <algorithm>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace edgevision {
namespace {
bool stable_device(const std::string& path) {
  for (const auto* prefix : {"/dev/v4l/by-id/", "/dev/v4l/by-path/"}) {
    const auto n = std::char_traits<char>::length(prefix);
    if (path.rfind(prefix, 0) == 0 && path.size() > n && path.find('/', n) == std::string::npos) return true;
  }
  return false;
}
}
bool ControlRouter::valid_config(const CaptureConfig& c) {
  if (c.device.empty() || c.device.size() > 256 || c.device.find('\0') != std::string::npos ||
      c.device.find("..") != std::string::npos) return false;
  const bool stable = stable_device(c.device);
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
  if (c.start_retry_limit > 2 || c.retry_delay_ms < 10 || c.retry_delay_ms > 2000) return false;
  if (c.runtime_retry_limit > 10 || c.runtime_retry_delay_ms < 50 || c.runtime_retry_delay_ms > 5000 ||
      c.runtime_recovery_budget_ms < c.runtime_retry_delay_ms || c.runtime_recovery_budget_ms > 60000) return false;
  const uint64_t conservative_bytes = uint64_t(c.width) * c.height * 2 * (c.driver_buffers + c.pool_slots);
  return conservative_bytes <= c.memory_budget_bytes;
}
std::string ControlRouter::canonical(const CaptureConfig& c) {
  std::ostringstream s;
  s << "capture-v2:" << c.device.size() << ':' << c.device << ':' << c.width << ':' << c.height << ':' << c.fourcc
    << ':' << c.fps_num << ':' << c.fps_den << ':' << c.driver_buffers << ':' << c.pool_slots << ':'
    << c.queue_capacity << ':' << c.memory_budget_bytes << ':' << c.allow_unstable_device
    << ':' << c.start_retry_limit << ':' << c.retry_delay_ms << ':' << c.runtime_retry_limit
    << ':' << c.runtime_retry_delay_ms << ':' << c.runtime_recovery_budget_ms;
  return s.str();
}
bool ControlRouter::terminal(TaskState state) {
  return state == TaskState::Done || state == TaskState::Failed || state == TaskState::Canceled || state == TaskState::Expired;
}
void ControlRouter::require_owner_locked() {
  if (!owner_) owner_ = std::this_thread::get_id();
  if (*owner_ != std::this_thread::get_id()) throw std::logic_error("ControlRouter requires one control owner");
}
ExecutionReply ControlRouter::initialize(InferenceConfig config) {
  { std::lock_guard<std::mutex> lock(mu_); require_owner_locked();
    if (shutting_down_) return {false, "RUNTIME_SHUTTING_DOWN"}; }
  return executor_ ? executor_->initialize(std::move(config)) : ExecutionReply{false, "NO_EXECUTOR"};
}
std::optional<TaskRecord> ControlRouter::collect(uint64_t id) const {
  auto record = tasks_.query(id);
  if (!record || !terminal(record->state) || record->execution_pending) return std::nullopt;
  return record;
}
void ControlRouter::cancel_pending(uint64_t now) {
  std::lock_guard<std::mutex> lock(mu_); tasks_.cancel_pending(now);
}
void ControlRouter::shutdown(uint64_t now) {
  std::optional<Action> action; std::optional<uint64_t> cancel;
  { std::lock_guard<std::mutex> lock(mu_); require_owner_locked();
    shutting_down_ = true; session_requested_ = false; state_.runtime_recovery_active = false; recovery_pending_ = Recovery::None;
    tasks_.cancel_pending(now);
    if (inference_task_) { tasks_.cancel(*inference_task_, now); cancel = inference_task_; execution_cancel_sent_ = true; }
    if (state_.active_task) tasks_.cancel(*state_.active_task, now);
    action = stop_locked("");
    if (!backend_live_) {
      if (state_.active_task) finish_locked(false, "", now);
      if (state_.capture!=CaptureState::Error) state_.capture=CaptureState::Idle;
      state_.first_frame.reset();
    }
  }
  execute(std::move(action), now);
  if (executor_) { if (cancel) executor_->cancel_pending(*cancel); executor_->shutdown(); }
}
TaskAdmission ControlRouter::submit(ControlRequest request, uint64_t now) {
  if (request.command != TaskCommand::StartCapture && request.command != TaskCommand::StopCapture &&
      !(request.command == TaskCommand::DetectSnapshot && executor_))
    return {false, false, 0, AdmissionError::UnsupportedCommand};
  if ((request.command == TaskCommand::StartCapture && (!request.capture || !valid_config(*request.capture))) ||
      (request.command != TaskCommand::StartCapture && request.capture))
    return {false, false, 0, AdmissionError::InvalidParameters};
  TaskRequest task{request.request_id, request.generation, request.command, request.deadline_mono_ns,
                   request.capture ? canonical(*request.capture) : request.command == TaskCommand::StopCapture ? "stop-v1" : "detect-snapshot-v1"};
  std::lock_guard<std::mutex> lock(mu_);
  const auto stop = pending_stop_ ? tasks_.query(*pending_stop_) : std::nullopt;
  const bool queued_stop = stop && stop->state == TaskState::Queued;
  const bool allow_new = !shutting_down_ && !queued_stop &&
      (request.command == TaskCommand::StopCapture || state_.capture != CaptureState::Stopping);
  auto result = tasks_.submit(task, now, state_.generation, allow_new);
  if (request.command == TaskCommand::StopCapture && result.error == AdmissionError::Busy && allow_new) {
    // A valid stop may replace waiting work, never a currently executing job.
    // submit already checked idempotency, generation, deadline and record budget.
    tasks_.cancel_pending(now); result = tasks_.submit(std::move(task), now, state_.generation);
  }
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
  snapshot.inference_task = inference_task_;
  if (!snapshot.active_task) snapshot.active_task = inference_task_;
  snapshot.shutting_down = shutting_down_;
  snapshot.shutdown_complete = shutting_down_ && !backend_live_ && !tasks_.execution_busy() && (!executor_ || executor_stopped_);
  return snapshot;
}
void ControlRouter::finish_locked(bool success, const std::string& error, uint64_t now) {
  if (state_.active_task) {
    const auto result = "{\"generation\":" + std::to_string(state_.generation) +
        ",\"start_retries\":" + std::to_string(state_.start_retries) +
        (state_.first_frame ? ",\"frame_id\":" + std::to_string(state_.first_frame->frame_id) : "") + "}";
    tasks_.complete(*state_.active_task, success, success ? result : "", error, now);
    configs_.erase(*state_.active_task);
    state_.active_task.reset();
  }
}
std::optional<ControlRouter::Action> ControlRouter::dispatch_locked(const TaskRecord& task, uint64_t now) {
  if (task.request.command == TaskCommand::DetectSnapshot) {
    if (state_.capture != CaptureState::Running || (state_.model != ModelState::Ready && state_.model != ModelState::Busy)) {
      tasks_.complete(task.id, false, "", state_.capture != CaptureState::Running ? "CAPTURE_NOT_RUNNING" : "MODEL_NOT_READY", now);
      return std::nullopt;
    }
    inference_task_ = task.id; execution_cancel_sent_ = false;
    return Action{Action::Kind::Detect, state_.generation, task.id, {}};
  }
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
    preempted_stop_.reset();
    state_.capture = CaptureState::Starting;
    state_.start_retries = state_.runtime_retries = 0; recovery_pending_ = Recovery::None;
    state_.runtime_recovery_active = false; runtime_deadline_ = 0; session_requested_ = false;
    state_.fault_detected_mono_ns = state_.recovered_mono_ns = 0;
    session_config_ = configs_.at(task.id);
    state_.first_frame.reset(); state_.last_error.clear(); error_after_cleanup_ = false;
    backend_live_ = true;
    return Action{Action::Kind::Start, next, task.id, configs_.at(task.id)};
  }
  // Explicit stop revokes automatic recovery, including while a worker cleans up.
  session_requested_ = false; state_.runtime_recovery_active = false; recovery_pending_ = Recovery::None;
  if (preempted_stop_ && *preempted_stop_ == task.id && !backend_live_ && error_after_cleanup_) {
    preempted_stop_.reset(); state_.capture = CaptureState::Error;
    finish_locked(false, state_.last_error, now); return std::nullopt;
  }
  preempted_stop_.reset();
  if (!backend_live_) {
    state_.capture = CaptureState::Idle; state_.last_error.clear(); state_.first_frame.reset();
    finish_locked(true, "", now); return std::nullopt;
  }
  // This explicit stop acknowledges the previous capture fault. Any subsequent
  // cleanup fault is still reported by the backend and fails the stop task.
  error_after_cleanup_ = false; state_.last_error.clear();
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
  if (event.kind == CaptureEvent::Kind::Fault) {
    if (state_.capture == CaptureState::Starting && state_.active_task && event.retryable) {
      const auto task = tasks_.query(*state_.active_task);
      const auto config = configs_.find(*state_.active_task);
      recovery_pending_ = task && task->state == TaskState::Running && config != configs_.end() &&
          state_.start_retries < config->second.start_retry_limit ? Recovery::Startup : Recovery::None;
    } else if ((state_.capture == CaptureState::Running || state_.runtime_recovery_active) &&
        session_requested_ && session_config_ && stable_device(session_config_->device) && event.runtime_retryable &&
        state_.runtime_retries < session_config_->runtime_retry_limit) {
      if (!state_.runtime_recovery_active) {
        state_.fault_detected_mono_ns = now;
        state_.recovered_mono_ns = 0;
        runtime_deadline_ = now + uint64_t(session_config_->runtime_recovery_budget_ms) * 1000000;
      }
      state_.runtime_recovery_active = true;
      recovery_pending_ = now < runtime_deadline_ ? Recovery::Runtime : Recovery::None;
    } else recovery_pending_ = Recovery::None;
    if (recovery_pending_ == Recovery::None) session_requested_ = false;
    state_.first_frame.reset();
    return stop_locked(event.error_code.empty() ? "CAPTURE_FAILED" : event.error_code);
  }
  if (event.kind == CaptureEvent::Kind::Stopped) {
    if (!backend_live_) return std::nullopt;
    if (state_.capture != CaptureState::Stopping) { error_after_cleanup_ = true; state_.last_error = "UNEXPECTED_STOP"; }
    backend_live_ = false;
    if (recovery_pending_ == Recovery::Runtime && session_requested_ && session_config_ && now < runtime_deadline_) {
      const auto delay_ms = std::min<uint64_t>(5000, uint64_t(session_config_->runtime_retry_delay_ms) << state_.runtime_retries);
      retry_at_ = now + delay_ms * 1000000;
      state_.capture = CaptureState::Recovering; recovery_pending_ = Recovery::None;
      return std::nullopt;
    }
    if (recovery_pending_ == Recovery::Startup && state_.active_task) {
      const auto task = tasks_.query(*state_.active_task);
      if (task && task->state == TaskState::Running) {
        ++state_.start_retries;
        retry_at_ = now + uint64_t(configs_.at(*state_.active_task).retry_delay_ms) * state_.start_retries * 1000000;
        state_.capture = CaptureState::Recovering; recovery_pending_ = Recovery::None;
        return std::nullopt;
      }
    }
    recovery_pending_ = Recovery::None; state_.runtime_recovery_active = false; session_requested_ = false;
    state_.capture = error_after_cleanup_ ? CaptureState::Error : CaptureState::Idle;
    state_.first_frame.reset();
    finish_locked(!error_after_cleanup_, state_.last_error, now);
    return std::nullopt;
  }
  if (state_.capture != CaptureState::Starting && !(state_.capture == CaptureState::Recovering && state_.runtime_recovery_active)) return std::nullopt;
  if (!session_config_) return stop_locked("MISSING_SESSION_CONFIG");
  if (state_.active_task) {
    auto task = tasks_.query(*state_.active_task);
    if (!task || terminal(task->state)) return stop_locked("");
  } else if (!state_.runtime_recovery_active || !session_requested_) return stop_locked("");
  const auto& frame = event.frame;
  const auto& config = *session_config_;
  bool valid = event.stream_on && frame.session_generation == state_.generation && frame.width && frame.height &&
      frame.width <= 8192 && frame.height <= 8192 && frame.bytesused && frame.host_dequeue_mono_ns &&
      frame.pixfmt_fourcc == config.fourcc && frame.bytesused <= config.memory_budget_bytes;
  if (frame.pixfmt_fourcc == make_fourcc('Y','U','Y','V'))
    valid = valid && frame.width % 2 == 0 && uint64_t(frame.stride) >= uint64_t(frame.width) * 2 &&
        frame.bytesused >= (uint64_t(frame.height) - 1) * frame.stride + uint64_t(frame.width) * 2;
  if (!valid) return stop_locked("INVALID_FIRST_FRAME");
  state_.first_frame = frame;
  state_.last_error.clear();
  state_.capture = CaptureState::Running;
  if (state_.runtime_recovery_active) state_.recovered_mono_ns = now;
  state_.runtime_recovery_active = false; session_requested_ = true;
  finish_locked(true, "", now);
  return std::nullopt;
}
void ControlRouter::execute(std::optional<Action> action, uint64_t now) {
  if (!action) return;
  if (action->kind == Action::Kind::Detect) {
    ExecutionReply reply;
    try { reply = executor_->submit({action->task, action->generation, tasks_.query(action->task)->request.deadline_mono_ns}); }
    catch (...) {
      // Possible partial acceptance: retain the physical slot until completion.
      std::lock_guard<std::mutex> lock(mu_);
      tasks_.fail_running(action->task, "EXECUTOR_SUBMIT_EXCEPTION", now);
      state_.model = ModelState::Error; state_.model_error = "EXECUTOR_SUBMIT_EXCEPTION"; return;
    }
    if (!reply.accepted) {
      std::lock_guard<std::mutex> lock(mu_);
      tasks_.complete(action->task, false, "", reply.error.empty() ? "EXECUTOR_REJECTED" : reply.error.substr(0, 128), now);
      inference_task_.reset();
    }
    return;
  }
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
    state_.runtime_recovery_active = false; session_requested_ = false; recovery_pending_ = Recovery::None;
    state_.last_error = reply.error_code.empty() ? "START_REJECTED" : reply.error_code.substr(0, 128);
    finish_locked(false, state_.last_error, now);
  }
}
std::optional<ControlRouter::Action> ControlRouter::reopen_locked(uint64_t now) {
  if (state_.capture != CaptureState::Recovering || backend_live_ || now < retry_at_) return std::nullopt;
  const bool runtime = state_.runtime_recovery_active && session_requested_ && session_config_;
  std::optional<TaskRecord> task;
  if (!runtime) {
    if (!state_.active_task) return std::nullopt;
    task = tasks_.query(*state_.active_task);
    if (!task || task->state != TaskState::Running) return std::nullopt;
  }
  const auto before = state_.generation;
  if (before == std::numeric_limits<uint64_t>::max() ||
      (!runtime && !tasks_.retarget_running(task->id, before, before + 1))) {
    state_.capture = CaptureState::Error; state_.last_error = "GENERATION_TRANSITION_FAILED";
    session_requested_ = false; state_.runtime_recovery_active = false;
    finish_locked(false, state_.last_error, now); return std::nullopt;
  }
  ++state_.generation; backend_live_ = true; error_after_cleanup_ = false;
  if (runtime) { ++state_.runtime_retries; state_.capture = CaptureState::Recovering; }
  else state_.capture = CaptureState::Starting;
  return Action{Action::Kind::Start, state_.generation, task ? task->id : 0, *session_config_};
}
void ControlRouter::observe_model(ModelState model, std::string error) {
  std::lock_guard<std::mutex> lock(mu_);
  require_owner_locked();
  state_.model = model;
  state_.model_error = error.substr(0, 128);
}
void ControlRouter::poll_executor(uint64_t now) {
  if (!executor_) return;
  try {
    const auto snapshot = executor_->query();
    { std::lock_guard<std::mutex> lock(mu_);
      state_.model = snapshot.model; state_.model_error = snapshot.error.substr(0, 128); executor_stopped_ = snapshot.stopped; }
    // Only one accepted execution and one bounded completion slot exist.
    auto completion = executor_->collect();
    if (!completion) return;
    std::lock_guard<std::mutex> lock(mu_);
    if (!inference_task_ || completion->task_id != *inference_task_) { ++state_.stale_events; return; }
    if (completion->generation != state_.generation)
      tasks_.fail_running(*inference_task_, "STALE_GENERATION", now);
    else if (state_.capture != CaptureState::Running)
      tasks_.fail_running(*inference_task_, "CAPTURE_NOT_RUNNING", now);
    tasks_.complete(*inference_task_, completion->success, std::move(completion->result), std::move(completion->error), now);
    inference_task_.reset(); execution_cancel_sent_ = false;
  } catch (...) {
    std::lock_guard<std::mutex> lock(mu_);
    state_.model = ModelState::Error; state_.model_error = "EXECUTOR_EVENT_EXCEPTION";
    if (inference_task_) tasks_.fail_running(*inference_task_, "EXECUTOR_EVENT_EXCEPTION", now);
  }
}
void ControlRouter::poll(uint64_t now) {
  std::optional<Action> action;
  std::optional<uint64_t> cancel_execution;
  {
    std::lock_guard<std::mutex> lock(mu_);
    require_owner_locked();
    tasks_.tick(now, state_.generation);
    if (pending_stop_) {
      const auto stop = tasks_.query(*pending_stop_);
      if (!stop || stop->state != TaskState::Queued) pending_stop_.reset();
      else {
        session_requested_ = false; state_.runtime_recovery_active = false; recovery_pending_ = Recovery::None;
        if (inference_task_ && preempted_stop_ != pending_stop_) {
          tasks_.cancel(*inference_task_, now); preempted_stop_ = pending_stop_;
          error_after_cleanup_ = false; state_.last_error.clear(); action = stop_locked("");
        }
        if ((state_.capture == CaptureState::Starting || state_.capture == CaptureState::Recovering) && state_.active_task) {
          // A queued stop preempts a no-frame start, but waits for physical cleanup.
          tasks_.cancel(*state_.active_task, now); action = stop_locked("");
        }
      }
    }
    if (inference_task_ && !execution_cancel_sent_) {
      const auto task = tasks_.query(*inference_task_);
      if (task && terminal(task->state)) { cancel_execution = inference_task_; execution_cancel_sent_ = true; }
    }
    // Expired/canceled queued configurations must not outlive their records.
    for (auto it = configs_.begin(); it != configs_.end();) {
      const auto record = tasks_.query(it->first);
      if (!record || (terminal(record->state) && !record->execution_pending)) it = configs_.erase(it); else ++it;
    }
    if (state_.active_task) {
      auto record = tasks_.query(*state_.active_task);
      if (record && terminal(record->state) && state_.capture == CaptureState::Starting) action = stop_locked("");
      if (record && terminal(record->state) && state_.capture == CaptureState::Recovering && !backend_live_) {
        recovery_pending_ = Recovery::None; state_.capture = CaptureState::Idle; state_.last_error.clear();
        finish_locked(false, "", now);
      }
    }
    if (state_.runtime_recovery_active && now >= runtime_deadline_) {
      session_requested_ = false; state_.runtime_recovery_active = false; recovery_pending_ = Recovery::None;
      if (backend_live_) action = stop_locked("RUNTIME_RECOVERY_TIMEOUT");
      else { state_.capture = CaptureState::Error; state_.last_error = "RUNTIME_RECOVERY_TIMEOUT"; }
    }
  }
  execute(std::move(action), now);
  if (cancel_execution) executor_->cancel_pending(*cancel_execution);
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
  poll_executor(now);
  {
    std::lock_guard<std::mutex> lock(mu_);
    // User commands win over a scheduled automatic reopen.
    if (!shutting_down_ && !state_.active_task && !tasks_.execution_busy()) {
      auto next = tasks_.take(now, state_.generation);
      if (next) action = dispatch_locked(*next, now);
    }
    if (!action) action = reopen_locked(now);
  }
  execute(std::move(action), now);
}
}  // namespace edgevision
