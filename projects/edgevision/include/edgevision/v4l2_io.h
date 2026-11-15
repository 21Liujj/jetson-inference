#pragma once
#include <cstddef>
#include <cstdint>

namespace edgevision {
// Personal J11 I/O seam, extended with an eventfd stop channel. Only LinuxIo
// touches real syscalls; deterministic contract tests inject an explicit fixture.
class V4L2Io {
 public:
  enum class Wait { Frame, Timeout, Stopped, Retry, Fault };
  virtual ~V4L2Io() = default;
  virtual int open_device(const char*) = 0;
  virtual int close_device(int) = 0;
  virtual int control(int, unsigned long, void*) = 0;
  virtual void* map_buffer(int, std::size_t, uint64_t) = 0;
  virtual int unmap_buffer(void*, std::size_t) = 0;
  virtual int create_stop_event() = 0;
  virtual void signal_stop(int) noexcept = 0;
  virtual Wait wait_frame_or_stop(int device, int stop, int timeout_ms) = 0;
  virtual uint64_t monotonic_ns() = 0;
};
}  // namespace edgevision
