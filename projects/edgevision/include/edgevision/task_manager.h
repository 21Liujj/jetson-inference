#pragma once
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

namespace edgevision {

enum class TaskCommand { StartCapture, StopCapture, CaptureSnapshot, DetectSnapshot,
                         DescribeSnapshot, GetRecentMetrics, CreateReport, RunInspection };
enum class TaskState { Queued, Running, Done, Failed, Expired, Canceled };
enum class AdmissionError { None, InvalidRequest, Busy, RecordBudget, Conflict,
                            StaleGeneration, DeadlineExceeded, UnsupportedCommand, InvalidParameters };

struct TaskRequest {
  std::string request_id;
  uint64_t generation = 0;
  TaskCommand command = TaskCommand::CaptureSnapshot;
  uint64_t deadline_mono_ns = 0;
  // Canonical, schema-validated parameters from the control boundary. Never
  // executed as shell text. The manager only compares and retains these bytes.
  std::string parameters;
};
struct TaskRecord {
  uint64_t id = 0;
  TaskRequest request;
  TaskState state = TaskState::Queued;
  uint64_t accepted_mono_ns = 0, started_mono_ns = 0, finished_mono_ns = 0;
  bool execution_pending = false;
  uint64_t execution_generation = 0;
  std::string error_code, result;
};
struct TaskAdmission {
  bool accepted = false, duplicate = false;
  uint64_t task_id = 0;
  AdmissionError error = AdmissionError::None;
};

// One active executor and a bounded waiting queue. Terminal state and physical
// completion are distinct: expiring/canceling RUNNING work never releases the
// active execution slot. A real backend calls complete() after safe completion.
// Records are never silently evicted, so a retained request_id never reexecutes.
// Reaching the bounded record budget explicitly rejects new work; durable
// history/retention is a later service-layer decision, not an unbounded map.
class TaskManager {
 public:
  explicit TaskManager(std::size_t pending_capacity = 1, std::size_t record_capacity = 256);
  TaskAdmission submit(TaskRequest, uint64_t now_mono_ns, uint64_t current_generation);
  std::optional<TaskRecord> take(uint64_t now_mono_ns, uint64_t current_generation);
  std::optional<TaskRecord> query(uint64_t id) const;
  bool cancel(uint64_t id, uint64_t now_mono_ns);
  bool complete(uint64_t id, bool success, std::string result, std::string error_code,
                uint64_t now_mono_ns);
  bool retarget_running(uint64_t id, uint64_t expected_generation, uint64_t next_generation);
  void tick(uint64_t now_mono_ns, uint64_t current_generation);
  void cancel_pending(uint64_t now_mono_ns);
  std::size_t queued() const;
  bool execution_busy() const;
  std::size_t records() const;

 private:
  void tick_locked(uint64_t now, uint64_t generation);
  static bool terminal(TaskState);
  static bool same_request(const TaskRequest&, const TaskRequest&);
  static bool valid_request(const TaskRequest&);
  mutable std::mutex mu_;
  const std::size_t pending_capacity_, record_capacity_;
  std::deque<uint64_t> queue_;
  std::optional<uint64_t> active_;
  std::unordered_map<uint64_t, TaskRecord> records_;
  std::unordered_map<std::string, uint64_t> by_request_;
  uint64_t next_id_ = 1;
};
}  // namespace edgevision
