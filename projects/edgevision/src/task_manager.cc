#include "edgevision/task_manager.h"
#include <algorithm>
#include <limits>
#include <stdexcept>
#include <utility>

namespace edgevision {
TaskManager::TaskManager(std::size_t pending, std::size_t records)
    : pending_capacity_(pending), record_capacity_(records) {
  if (!pending || !records || pending > records || records > 65536)
    throw std::invalid_argument("invalid task queue/record budget");
}
bool TaskManager::terminal(TaskState s) {
  return s == TaskState::Done || s == TaskState::Failed || s == TaskState::Expired || s == TaskState::Canceled;
}
bool TaskManager::valid_request(const TaskRequest& r) {
  if (r.request_id.empty() || r.request_id.size() > 64 || r.parameters.size() > 2048 || !r.deadline_mono_ns)
    return false;
  if (r.command < TaskCommand::StartCapture || r.command > TaskCommand::RunInspection) return false;
  for (unsigned char c : r.request_id)
    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
          c == '-' || c == '_' || c == '.' || c == ':')) return false;
  return true;
}
bool TaskManager::same_request(const TaskRequest& a, const TaskRequest& b) {
  // A retry cannot extend the original deadline. Return the original record.
  return a.command == b.command && a.generation == b.generation && a.parameters == b.parameters;
}
TaskAdmission TaskManager::submit(TaskRequest request, uint64_t now, uint64_t generation, bool allow_new) {
  if (!valid_request(request)) return {false, false, 0, AdmissionError::InvalidRequest};
  std::lock_guard<std::mutex> lock(mu_);
  tick_locked(now, generation);
  auto old = by_request_.find(request.request_id);
  if (old != by_request_.end()) {
    const auto& record = records_.at(old->second);
    if (!same_request(record.request, request)) return {false, false, record.id, AdmissionError::Conflict};
    return {true, true, record.id, AdmissionError::None};
  }
  if (request.generation != generation) return {false, false, 0, AdmissionError::StaleGeneration};
  if (now >= request.deadline_mono_ns) return {false, false, 0, AdmissionError::DeadlineExceeded};
  if (!allow_new) return {false, false, 0, AdmissionError::Busy};
  if (records_.size() >= record_capacity_ || next_id_ == std::numeric_limits<uint64_t>::max())
    return {false, false, 0, AdmissionError::RecordBudget};
  if (queue_.size() >= pending_capacity_) return {false, false, 0, AdmissionError::Busy};
  TaskRecord record;
  record.id = next_id_++;
  record.request = std::move(request);
  record.execution_generation = record.request.generation;
  record.accepted_mono_ns = now;
  by_request_.emplace(record.request.request_id, record.id);
  records_.emplace(record.id, record);
  queue_.push_back(record.id);
  return {true, false, record.id, AdmissionError::None};
}
void TaskManager::tick_locked(uint64_t now, uint64_t generation) {
  for (auto id : queue_) {
    auto& record = records_.at(id);
    if (record.request.generation != generation) {
      record.state = TaskState::Failed;
      record.error_code = "STALE_GENERATION";
      record.finished_mono_ns = now;
    } else if (now >= record.request.deadline_mono_ns) {
      record.state = TaskState::Expired;
      record.error_code = "DEADLINE_EXCEEDED";
      record.finished_mono_ns = now;
    }
  }
  queue_.erase(std::remove_if(queue_.begin(), queue_.end(), [&](uint64_t id) {
    return terminal(records_.at(id).state);
  }), queue_.end());
  if (active_) {
    auto& record = records_.at(*active_);
    if (!terminal(record.state) && (record.execution_generation != generation || now >= record.request.deadline_mono_ns)) {
      record.state = record.execution_generation != generation ? TaskState::Failed : TaskState::Expired;
      record.error_code = record.execution_generation != generation ? "STALE_GENERATION" : "DEADLINE_EXCEEDED";
      record.finished_mono_ns = now;
      // Preserve execution_pending/active_ until complete(): GPU may still run.
    }
  }
}
void TaskManager::tick(uint64_t now, uint64_t generation) {
  std::lock_guard<std::mutex> lock(mu_); tick_locked(now, generation);
}
std::optional<TaskRecord> TaskManager::take(uint64_t now, uint64_t generation) {
  std::lock_guard<std::mutex> lock(mu_);
  tick_locked(now, generation);
  if (active_ || queue_.empty()) return std::nullopt;
  const auto id = queue_.front(); queue_.pop_front();
  auto& record = records_.at(id);
  record.state = TaskState::Running;
  record.started_mono_ns = now;
  record.execution_pending = true;
  active_ = id;
  return record;
}
std::optional<TaskRecord> TaskManager::query(uint64_t id) const {
  std::lock_guard<std::mutex> lock(mu_);
  auto it = records_.find(id);
  if (it == records_.end()) return std::nullopt;
  return it->second;
}
bool TaskManager::cancel(uint64_t id, uint64_t now) {
  std::lock_guard<std::mutex> lock(mu_);
  auto it = records_.find(id);
  if (it == records_.end()) return false;
  auto& record = it->second;
  if (terminal(record.state)) return true;
  record.state = TaskState::Canceled;
  record.error_code = "CANCELED";
  record.finished_mono_ns = now;
  queue_.erase(std::remove(queue_.begin(), queue_.end(), id), queue_.end());
  return true;
}
bool TaskManager::fail_running(uint64_t id, std::string error, uint64_t now) {
  std::lock_guard<std::mutex> lock(mu_);
  if (!active_ || *active_ != id || error.empty() || error.size() > 128) return false;
  auto& record = records_.at(id);
  if (!terminal(record.state)) {
    record.state = TaskState::Failed; record.error_code = std::move(error); record.finished_mono_ns = now;
  }
  return true; // physical ownership is retained until complete()
}
bool TaskManager::complete(uint64_t id, bool success, std::string result, std::string error, uint64_t now) {
  std::lock_guard<std::mutex> lock(mu_);
  if (!active_ || *active_ != id) return false;
  auto& record = records_.at(id);
  if (!terminal(record.state)) {
    if (now >= record.request.deadline_mono_ns) {
      record.state = TaskState::Expired; record.error_code = "DEADLINE_EXCEEDED";
    } else if (result.size() > 16384 || error.size() > 128) {
      record.state = TaskState::Failed; record.error_code = "RESULT_BUDGET_EXCEEDED";
    } else {
      record.state = success ? TaskState::Done : TaskState::Failed;
      record.error_code = success ? "" : (error.empty() ? "BACKEND_FAILED" : std::move(error));
      record.result = std::move(result);
    }
    record.finished_mono_ns = now;
  }
  record.execution_pending = false;
  active_.reset();
  return true;
}
bool TaskManager::retarget_running(uint64_t id, uint64_t expected, uint64_t next) {
  std::lock_guard<std::mutex> lock(mu_);
  if (!active_ || *active_ != id) return false;
  auto& record = records_.at(id);
  if (record.state != TaskState::Running || record.execution_generation != expected || next <= expected) return false;
  record.execution_generation = next;
  return true;
}
void TaskManager::cancel_pending(uint64_t now) {
  std::lock_guard<std::mutex> lock(mu_);
  for (auto id : queue_) {
    auto& record = records_.at(id);
    record.state = TaskState::Canceled; record.error_code = "CANCELED"; record.finished_mono_ns = now;
  }
  queue_.clear();
}
std::size_t TaskManager::queued() const { std::lock_guard<std::mutex> lock(mu_); return queue_.size(); }
bool TaskManager::execution_busy() const { std::lock_guard<std::mutex> lock(mu_); return active_.has_value(); }
std::size_t TaskManager::records() const { std::lock_guard<std::mutex> lock(mu_); return records_.size(); }
bool TaskManager::release_terminal(uint64_t id) {
  std::lock_guard<std::mutex> lock(mu_);
  auto it=records_.find(id);
  if(it==records_.end() || !terminal(it->second.state) || it->second.execution_pending ||
     (active_ && *active_==id)) return false;
  by_request_.erase(it->second.request.request_id); records_.erase(it); return true;
}
}  // namespace edgevision
