#include "edgevision/frame_queue.h"
#include <cassert>
#include <cstdio>
#include <future>
using namespace edgevision;
static FrameLease ready(FramePool& pool, uint8_t value, uint64_t generation) {
  int slot = pool.acquire_index(); assert(slot >= 0);
  auto& frame = pool.slot_ref(slot);
  frame.data = {value}; frame.bytesused = 1; frame.session_generation = generation; frame.valid = true;
  FrameLease lease(&pool, slot); assert(lease.mark_ready(100)); return lease;
}
int main() {
  FramePool pool(4, 64); FrameQueue queue(7);
  assert(queue.push(ready(pool, 1, 7)) == FrameQueue::Push::Accepted);
  assert(queue.push(ready(pool, 2, 7)) == FrameQueue::Push::Accepted);
  assert(queue.push(ready(pool, 3, 7)) == FrameQueue::Push::ReplacedOldest);
  assert(queue.depth() == 2 && queue.replaced_oldest() == 1 && pool.busy() == 2);
  FrameLease out; assert(queue.consume(out, 0) && out->data[0] == 2); out.release();
  assert(queue.consume(out, 0) && out->data[0] == 3); out.release();
  auto stale = ready(pool, 4, 6);
  assert(queue.push(std::move(stale)) == FrameQueue::Push::StaleGeneration && stale.valid());
  stale.release();
  {
    class Completed final : public CompletionFence {
      bool ready() const noexcept override { return true; }
      void wait() noexcept override {}
    };
    auto in_flight = ready(pool, 5, 7);
    assert(in_flight.mark_inflight(std::make_shared<Completed>()));
    assert(queue.push(std::move(in_flight)) == FrameQueue::Push::InvalidState);
    assert(in_flight.valid() && in_flight->data[0] == 5 && pool.busy() == 1);
    assert(in_flight.complete_if_ready() && in_flight.state() == SlotState::Done);
    in_flight.release();
  }
  assert(queue.push(ready(pool, 6, 7)) == FrameQueue::Push::Accepted);
  queue.close(); assert(pool.busy() == 0 && !queue.consume(out, -1));
  assert(queue.push(ready(pool, 7, 7)) == FrameQueue::Push::Stopped && pool.busy() == 0);
  // A stopped queue wakes every waiting consumer, including indefinite waits.
  FrameQueue empty(8); auto consumer = std::async(std::launch::async, [&] { FrameLease f; return empty.consume(f, -1); });
  empty.close(); assert(consumer.wait_for(std::chrono::seconds(1)) == std::future_status::ready && !consumer.get());
  std::puts("PASS READY-only freshness queue, generation and in-flight exclusion; CPU fixtures only");
}
