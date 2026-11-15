#include "edgevision/task_manager.h"
#include <cassert>
#include <future>
#include <cstdio>
#include <vector>
using namespace edgevision;
static TaskRequest request(std::string id, uint64_t deadline = 100, uint64_t generation = 1) {
  return {std::move(id), generation, TaskCommand::CaptureSnapshot, deadline, "{}"};
}
int main() {
  {
    TaskManager manager;
    auto a = manager.submit(request("a"), 1, 1);
    assert(a.accepted && !a.duplicate);
    auto retry = manager.submit(request("a", 999), 2, 1);
    assert(retry.accepted && retry.duplicate && retry.task_id == a.task_id);
    assert(manager.query(a.task_id)->request.deadline_mono_ns == 100);
    auto conflict = request("a"); conflict.parameters = "different";
    assert(manager.submit(conflict, 3, 1).error == AdmissionError::Conflict);
    assert(manager.submit(request("b"), 4, 1).error == AdmissionError::Busy);
    auto running = manager.take(5, 1);
    assert(running && running->id == a.task_id && running->state == TaskState::Running);
    assert(manager.submit(request("b", 200), 6, 1).accepted);
    manager.tick(101, 1);
    assert(manager.query(a.task_id)->state == TaskState::Expired);
    assert(manager.execution_busy() && manager.query(a.task_id)->execution_pending);
    assert(!manager.take(102, 1));
    assert(manager.complete(a.task_id, true, "late result", "", 103));
    assert(manager.query(a.task_id)->state == TaskState::Expired && !manager.execution_busy());
    auto b = manager.take(104, 1); assert(b && b->request.request_id == "b");
    assert(manager.cancel(b->id, 105) && manager.execution_busy());
    assert(manager.complete(b->id, true, "must not publish", "", 106));
    assert(manager.query(b->id)->state == TaskState::Canceled && manager.query(b->id)->result.empty());
    assert(!manager.complete(b->id, true, "twice", "", 107));
  }
  {
    TaskManager manager(2, 3);
    auto a = manager.submit(request("old"), 1, 1);
    assert(a.accepted && manager.take(2, 1));
    auto queued = manager.submit(request("queued"), 3, 1); assert(queued.accepted);
    manager.tick(4, 2);
    assert(manager.query(a.task_id)->error_code == "STALE_GENERATION" && manager.execution_busy());
    assert(manager.query(queued.task_id)->state == TaskState::Failed && manager.queued() == 0);
    assert(manager.complete(a.task_id, true, "old", "", 5));
    assert(manager.query(a.task_id)->result.empty());
    auto fresh = manager.submit(request("fresh", 100, 2), 6, 2); assert(fresh.accepted);
    manager.cancel_pending(7);
    assert(manager.query(fresh.task_id)->state == TaskState::Canceled);
    assert(manager.submit(request("full", 100, 2), 8, 2).error == AdmissionError::RecordBudget);
    assert(manager.submit(request("fresh", 100, 2), 9, 2).duplicate);
  }
  {
    TaskManager manager;
    assert(manager.submit(request("bad id"), 1, 1).error == AdmissionError::InvalidRequest);
    assert(manager.submit(request("expired", 1), 1, 1).error == AdmissionError::DeadlineExceeded);
    assert(manager.submit(request("stale", 100, 0), 1, 1).error == AdmissionError::StaleGeneration);
    auto a = manager.submit(request("a"), 1, 1); assert(manager.take(2, 1));
    assert(manager.complete(a.task_id, true, std::string(16385, 'x'), "", 3));
    assert(manager.query(a.task_id)->error_code == "RESULT_BUDGET_EXCEEDED");
  }
  // Concurrent retries admit exactly one task, not one task per API entry.
  {
    TaskManager manager;
    std::vector<std::future<TaskAdmission>> clients;
    for (int i = 0; i < 16; ++i) clients.push_back(std::async(std::launch::async, [&] {
      return manager.submit(request("same"), 1, 1);
    }));
    uint64_t id = 0; unsigned first = 0;
    for (auto& client : clients) { auto a = client.get(); assert(a.accepted); if (!a.duplicate) ++first;
      if (!id) id = a.task_id; assert(id == a.task_id); }
    assert(first == 1 && manager.records() == 1 && manager.queued() == 1);
  }
  std::puts("PASS task admission/idempotency/deadline/cancel/generation/budget; no hardware executed");
}
