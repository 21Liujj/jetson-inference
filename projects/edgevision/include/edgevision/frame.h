#pragma once
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace edgevision {
namespace detail {struct FramePoolState;}
class FrameLease;

struct FrameMeta {
  enum class TimestampClock { Unknown, Monotonic };
  enum class TimestampSource { Unknown, EndOfFrame, StartOfExposure };
  uint64_t host_dequeue_mono_ns = 0;
  uint64_t publish_mono_ns = 0;
  uint64_t session_generation = 0;
  // Keep host DQBUF receipt separate from the driver-provided capture time.
  std::optional<uint64_t> driver_timestamp_ns;
  TimestampClock driver_timestamp_clock=TimestampClock::Unknown;
  TimestampSource driver_timestamp_source=TimestampSource::Unknown;
  uint32_t driver_sequence=0,driver_flags=0;
  uint64_t frame_id = 0;
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t pixfmt_fourcc = 0;  // V4L2 fourcc, e.g. 'MJPG'
  uint32_t stride = 0;
  uint32_t bytesused = 0;
};

struct Frame : FrameMeta {
  std::vector<uint8_t> data;
  uint64_t epoch = 0;
  bool valid = false;
};

enum class SlotState { Free, Writing, Ready, InFlight, Done };

class FramePool {
 public:
  // Producer owns an acquired slot until release or move-only lease handoff.
  // Leases retain storage, not this handle. Never share one lease object
  // concurrently, or use a raw slot_ref after handing that slot to a lease.
  FramePool(std::size_t slots, std::size_t capacity);
  ~FramePool() = default;
  FramePool(const FramePool&) = delete;
  FramePool& operator=(const FramePool&) = delete;

  int acquire_index();
  Frame& slot_ref(int slot);
  int acquire(Frame& out);
  void release(int slot);
  uint64_t epoch_of(int slot) const;
  std::size_t busy() const;
  std::size_t slots() const;
  std::size_t slots_in_state(SlotState state) const;

 private:
  friend class FrameLease;
  static uint64_t lease_epoch(const std::shared_ptr<detail::FramePoolState>&,int);
  static Frame* lease_frame(const std::shared_ptr<detail::FramePoolState>&,int,uint64_t);
  static void release_lease(const std::shared_ptr<detail::FramePoolState>&,int,uint64_t);
  static SlotState lease_state(const std::shared_ptr<detail::FramePoolState>&,int,uint64_t);
  static bool transition(const std::shared_ptr<detail::FramePoolState>&,int,uint64_t,SlotState,SlotState);
  std::shared_ptr<detail::FramePoolState> state_;
};

}  // namespace edgevision
