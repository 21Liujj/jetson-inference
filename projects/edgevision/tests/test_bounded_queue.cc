// Minimal smoke test for BoundedQueue: capacity, drop counting, shutdown.
// No hardware: must pass on any host via `ctest`.
#include <cassert>
#include <cstdio>
#include <string>
#include <atomic>
#include <future>
#include <functional>
#include <memory>
#include <vector>
#include "edgevision/bounded_queue.h"

int main() {
  edgevision::BoundedQueue<int> q(2);
  assert(q.try_push(1));
  assert(q.try_push(2));
  assert(!q.try_push(3));  // full: drop, never block
  assert(q.dropped() == 1);

  int v = 0;
  assert(q.wait_pop(v, 100) && v == 1);
  assert(q.wait_pop(v, 100) && v == 2);
  assert(!q.wait_pop(v, 50));  // empty: timeout, not hang

  q.shutdown();
  assert(!q.try_push(4));
  assert(q.snapshot().rejected_stopped==1&&q.dropped()==1);
  assert(!q.wait_pop(v, 50));  // shutdown wakes waiter with false
  // Every blocked consumer is woken, including indefinite waits.
  {
    edgevision::BoundedQueue<int> stopped(2);
    std::vector<std::future<bool>> readers;
    for(unsigned i=0;i<4;++i)readers.emplace_back(std::async(std::launch::async,[&]{int value;return stopped.wait_pop(value,-1);}));
    for(auto& r:readers)assert(r.wait_for(std::chrono::milliseconds(20))==std::future_status::timeout);
    stopped.shutdown();stopped.shutdown();
    for(auto& r:readers){assert(r.wait_for(std::chrono::seconds(1))==std::future_status::ready);assert(!r.get());}
  }
  // Drain remains FIFO; abort discards pending, cannot reopen, and is idempotent.
  {
    using Queue=edgevision::BoundedQueue<int>;Queue drain(4);
    assert(drain.try_push(1)&&drain.try_push(2)&&drain.try_push(3));drain.shutdown();
    assert(drain.wait_pop(v,-1)&&v==1);drain.shutdown(Queue::StopMode::Abort);
    assert(!drain.wait_pop(v,-1)&&!drain.try_push(4));drain.shutdown(Queue::StopMode::Drain);
    auto s=drain.snapshot();assert(s.enqueued==3&&s.dequeued==1&&s.aborted==2&&s.depth==0&&s.high_water==3);
  }
  // Rejected/aborted resources and the previous output are destroyed outside
  // the queue lock: destruction may reenter size(), as FrameLease release may.
  {
    struct Resource {std::function<void()> release;~Resource(){release();}};
    using Item=std::unique_ptr<Resource>;using Queue=edgevision::BoundedQueue<Item>;
    Queue resources(1);std::atomic<unsigned> released{0};
    // Allocate directly; a temporary Resource would itself run its destructor.
    auto owned=[&]{return Item(new Resource{[&]{++released;(void)resources.size();}});};
    assert(resources.try_push(owned()));assert(!resources.try_push(owned()));assert(released==1);
    Item out=owned();assert(resources.wait_pop(out,0));assert(released==2);out.reset();assert(released==3);
    assert(resources.try_push(owned()));resources.shutdown(Queue::StopMode::Abort);assert(released==4);
    assert(!resources.try_push(owned()));assert(released==5);
  }
  // Producer/stop race: each input has exactly one accounted outcome.
  {
    edgevision::BoundedQueue<int> race(32);std::promise<void> go;auto start=go.get_future().share();
    std::vector<std::future<void>> writers;
    for(unsigned p=0;p<4;++p)writers.emplace_back(std::async(std::launch::async,[&,p]{start.wait();for(unsigned i=0;i<5000;++i)race.try_push(p*5000+i);}));
    go.set_value();race.shutdown();
    for(auto& w:writers)w.get();
    auto s=race.snapshot();assert(s.enqueued+s.dropped_full+s.rejected_stopped==20000);
    assert(s.enqueued==s.depth&&s.depth<=32);
    while(race.wait_pop(v,0)){}
    s=race.snapshot();assert(s.enqueued==s.dequeued+s.aborted+s.depth&&s.depth==0);
  }
  // New M2 freshness policy discards only the oldest waiting item.
  {
    using Queue=edgevision::BoundedQueue<int>;Queue latest(2);
    assert(latest.push_latest(1)==Queue::PushResult::Enqueued);
    assert(latest.push_latest(2)==Queue::PushResult::Enqueued);
    assert(latest.push_latest(3)==Queue::PushResult::ReplacedOldest);
    assert(latest.wait_pop(v,0)&&v==2);
    assert(latest.wait_pop(v,0)&&v==3);
    auto s=latest.snapshot();assert(s.enqueued==3&&s.dequeued==2&&latest.replaced_oldest()==1);
    latest.shutdown();assert(latest.push_latest(4)==Queue::PushResult::Stopped);
  }
  {
    bool threw=false;try{edgevision::BoundedQueue<int> invalid(0);}catch(const std::invalid_argument&){threw=true;}
    assert(threw);
    struct Resource{std::function<void()> release;~Resource(){release();}};
    using Item=std::unique_ptr<Resource>;using Queue=edgevision::BoundedQueue<Item>;Queue latest(1);
    unsigned releases=0;
    auto item=[&]{return Item(new Resource{[&]{++releases;(void)latest.size();}});};
    assert(latest.push_latest(item())==Queue::PushResult::Enqueued);
    assert(latest.push_latest(item())==Queue::PushResult::ReplacedOldest);
    assert(releases==1);latest.shutdown(Queue::StopMode::Abort);assert(releases==2);
  }
  std::printf("[test_bounded_queue] PASS\n");
  return 0;
}
