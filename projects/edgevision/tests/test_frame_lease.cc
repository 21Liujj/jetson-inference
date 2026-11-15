#include <cassert>
#include <cstdio>
#include <future>
#include <memory>
#include <vector>
#include <atomic>
#include <condition_variable>
#include "edgevision/bounded_queue.h"
#include "edgevision/frame.h"
#include "edgevision/frame_lease.h"

int main() {
  edgevision::FramePool pool(4, 1024 * 1024);

  {
    int s0 = pool.acquire_index();
    assert(s0 >= 0);
    edgevision::Frame& f0 = pool.slot_ref(s0);
    f0.data.assign(100, 'x');
    f0.valid = true;

    {
      edgevision::FrameLease lease(&pool, s0);
      assert(lease.valid());
      assert(lease->data.size() == 100);
      assert(lease.slot() == s0);
    }

    assert(!pool.slot_ref(s0).valid);
  }

  {
    int s1 = pool.acquire_index();
    assert(s1 >= 0);
    edgevision::Frame& f1 = pool.slot_ref(s1);
    f1.data.assign(50, 'y');
    f1.valid = true;

    edgevision::FrameLease a(&pool, s1);
    uint64_t a_epoch = pool.epoch_of(s1);

    edgevision::FrameLease b(std::move(a));
    assert(!a.valid());
    assert(b.valid());
    assert(b->data.size() == 50);

    b.release();
  }

  {
    int sa = pool.acquire_index();
    int sb = pool.acquire_index();
    assert(sa >= 0 && sb >= 0);
    pool.slot_ref(sa).valid = true;
    pool.slot_ref(sb).valid = true;

    edgevision::FrameLease la(&pool, sa);
    edgevision::FrameLease lb(&pool, sb);

    la = std::move(lb);
    assert(!lb.valid());
    assert(la.valid());
    assert(la.slot() == sb);
  }

  {
    int sx = pool.acquire_index();
    assert(sx >= 0);
    pool.slot_ref(sx).valid = true;
    uint64_t e1 = pool.epoch_of(sx);

    edgevision::FrameLease lx(&pool, sx);
    assert(lx.valid());

    pool.release(sx);
    int sy = pool.acquire_index();
    assert(sy == sx);
    pool.slot_ref(sy).valid = true;

    assert(!lx.valid());
    lx.release();assert(pool.busy()==1); // stale release must not free sy
    pool.release(sy);assert(pool.busy()==0);
  }

  // Storage ownership survives destruction/replacement of the pool handle.
  edgevision::FrameLease held;
  {
    auto original=std::make_unique<edgevision::FramePool>(1,64);
    int slot=original->acquire_index();original->slot_ref(slot).data={42};original->slot_ref(slot).valid=true;
    held=edgevision::FrameLease(original.get(),slot);original.reset();
  }
  assert(held.valid()&&held->data.at(0)==42);
  auto replacement=std::make_unique<edgevision::FramePool>(1,64);
  int new_slot=replacement->acquire_index();replacement->slot_ref(new_slot).data={99};replacement->slot_ref(new_slot).valid=true;
  edgevision::FrameLease current(replacement.get(),new_slot);
  assert(current.valid()&&current->data.at(0)==99&&held->data.at(0)==42);
  held.release();assert(current.valid()&&replacement->busy()==1);current.release();assert(replacement->busy()==0);

  // Queued leases own resources before consumer pop, including pool destruction.
  {
    using Queue=edgevision::BoundedQueue<edgevision::FrameLease>;Queue queue(1);
    auto producer=std::make_unique<edgevision::FramePool>(1,64);
    int slot=producer->acquire_index();producer->slot_ref(slot).data={17};producer->slot_ref(slot).valid=true;
    assert(queue.try_push(edgevision::FrameLease(producer.get(),slot)));producer.reset();
    auto reader=std::async(std::launch::async,[&]{edgevision::FrameLease frame;assert(queue.wait_pop(frame,-1));assert(frame.valid()&&frame->data.at(0)==17);});
    reader.get();queue.shutdown(Queue::StopMode::Abort);
  }
  // Dropped/aborted leases return exactly their own epoch's slots.
  {
    using Queue=edgevision::BoundedQueue<edgevision::FrameLease>;Queue queue(1);edgevision::FramePool resources(2,64);
    int a=resources.acquire_index(),b=resources.acquire_index();resources.slot_ref(a).valid=resources.slot_ref(b).valid=true;
    assert(queue.try_push(edgevision::FrameLease(&resources,a)));
    const bool accepted=queue.try_push(edgevision::FrameLease(&resources,b));
    assert(!accepted&&resources.busy()==1); // inspect after full-expression destruction
    queue.shutdown(Queue::StopMode::Abort);assert(resources.busy()==0);
  }
  // Many old generations may be held while new pools are made and destroyed.
  {
    std::vector<edgevision::FrameLease> generations;
    for(unsigned i=0;i<1000;++i){
      edgevision::FramePool temporary(1,64);int slot=temporary.acquire_index();
      temporary.slot_ref(slot).data={static_cast<uint8_t>(i)};temporary.slot_ref(slot).valid=true;
      generations.emplace_back(&temporary,slot);
    }
    for(unsigned i=0;i<1000;++i)assert(generations[i].valid()&&generations[i]->data.at(0)==static_cast<uint8_t>(i));
  }
  edgevision::FrameLease empty;assert(!empty.valid()&&empty.operator->()==nullptr);
  bool threw=false;try{(void)*empty;}catch(const std::logic_error&){threw=true;}assert(threw);

  // A logical timeout cannot return an INFLIGHT slot. The completion fence
  // here is an explicit CPU test fixture, not a simulated hardware pass.
  {
    class ManualFence final:public edgevision::CompletionFence {
     public:
      std::atomic<bool> done{false};std::mutex mu;std::condition_variable cv;std::promise<void> entered;
      bool ready()const noexcept override{return done.load();}
      void wait()noexcept override{entered.set_value();std::unique_lock<std::mutex> lock(mu);cv.wait(lock,[&]{return done.load();});}
      void signal(){{std::lock_guard<std::mutex> lock(mu);done.store(true);}cv.notify_all();}
    };
    edgevision::FramePool one(1,64);int index=one.acquire_index();
    auto& frame=one.slot_ref(index);frame.data={41};frame.valid=true;frame.session_generation=7;
    edgevision::FrameLease held(&one,index);assert(held.state()==edgevision::SlotState::Writing);
    assert(held.mark_ready(100)&&held->publish_mono_ns==100);
    assert(!held.mark_ready(101));
    auto fence=std::make_shared<ManualFence>();auto entered=fence->entered.get_future();
    assert(held.mark_inflight(fence)&&!held.complete_if_ready());
    auto release=std::async(std::launch::async,[owned=std::move(held)]()mutable{owned.release();});
    assert(entered.wait_for(std::chrono::seconds(1))==std::future_status::ready);
    assert(one.slots_in_state(edgevision::SlotState::InFlight)==1&&one.acquire_index()==-1);
    one.release(index);assert(one.busy()==1); // raw release cannot override a completion fence
    assert(release.wait_for(std::chrono::milliseconds(20))==std::future_status::timeout);
    fence->signal();release.get();assert(one.busy()==0);
  }
  {
    bool refused=false;try{edgevision::FramePool too_large(1025,64);}catch(const std::invalid_argument&){refused=true;}
    assert(refused);
    edgevision::FramePool one(1,64);int index=one.acquire_index();
    one.slot_ref(index).session_generation=99;one.slot_ref(index).stride=1280;
    one.release(index);index=one.acquire_index();
    assert(one.slot_ref(index).session_generation==0&&one.slot_ref(index).stride==0);one.release(index);
  }
  std::printf("[test_frame_lease] PASS\n");
  return 0;
}
