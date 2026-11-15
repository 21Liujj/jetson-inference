// Personal J11 V4L2/MMAP port. See docs/capture-provenance.json for donor hashes.
#include "edgevision/v4l2_capture.h"
#include <fcntl.h>
#include <linux/videodev2.h>
#include <poll.h>
#include <sys/eventfd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <cerrno>
#include <algorithm>
#include <cstring>
#include <ctime>
#include <limits>

namespace edgevision {
namespace {
constexpr uint32_t max_image = 64u * 1024 * 1024;
class LinuxIo final : public V4L2Io {
 public:
  int open_device(const char* path) override { return ::open(path, O_RDWR | O_NONBLOCK | O_CLOEXEC); }
  int close_device(int fd) override { return ::close(fd); }
  int control(int fd, unsigned long op, void* arg) override { return ::ioctl(fd, op, arg); }
  void* map_buffer(int fd, std::size_t n, uint64_t offset) override {
    return ::mmap(nullptr, n, PROT_READ | PROT_WRITE, MAP_SHARED, fd, static_cast<off_t>(offset));
  }
  int unmap_buffer(void* p, std::size_t n) override { return ::munmap(p, n); }
  int create_stop_event() override { return ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC); }
  void signal_stop(int fd) noexcept override {
    const uint64_t value = 1;
    for (unsigned i = 0; i < 8; ++i) {
      if (::write(fd, &value, sizeof(value)) >= 0 || errno != EINTR) return;
    }
  }
  Wait wait_frame_or_stop(int fd, int stop, int ms) override {
    pollfd p[2]{{fd, POLLIN | POLLRDNORM, 0}, {stop, POLLIN, 0}};
    const int n = ::poll(p, 2, ms);
    if (n < 0) return errno == EINTR ? Wait::Retry : Wait::Fault;
    if (!n) return Wait::Timeout;
    if (p[1].revents & POLLIN) return Wait::Stopped;
    if ((p[0].revents | p[1].revents) & (POLLERR | POLLHUP | POLLNVAL)) { errno = ENODEV; return Wait::Fault; }
    return p[0].revents & (POLLIN | POLLRDNORM) ? Wait::Frame : Wait::Retry;
  }
  uint64_t monotonic_ns() override {
    timespec t{}; ::clock_gettime(CLOCK_MONOTONIC, &t);
    return uint64_t(t.tv_sec) * 1000000000 + t.tv_nsec;
  }
};
std::string error(const char* op) { return std::string(op) + ": " + std::strerror(errno); }
}
V4L2Capture::V4L2Capture(std::shared_ptr<V4L2Io> io) : io_(io ? std::move(io) : std::make_shared<LinuxIo>()) {}
int V4L2Capture::control(unsigned long op, void* arg) {
  for (unsigned i = 0; i < 8; ++i) {
    int n = io_->control(fd_, op, arg);
    if (n != -1 || errno != EINTR) return n;
  }
  return -1;
}
bool V4L2Capture::open(const std::string& device) {
  close(); last_error_.clear(); warning_.clear(); stop_requested_.store(false);
  last_errno_ = 0;
  if (!cleanup_complete()) { last_error_ = "PREVIOUS_MAPPING_CLEANUP_PENDING"; return false; }
  counters_ = {}; format_ = {}; last_sequence_.reset(); generation_ = frame_id_ = 0;
  fd_ = io_->open_device(device.c_str());
  if (fd_ < 0) { last_errno_ = errno; last_error_ = error("OPEN"); return false; }
  stop_fd_ = io_->create_stop_event();
  if (stop_fd_ < 0) { last_errno_ = errno; last_error_ = error("EVENTFD"); io_->close_device(fd_); fd_ = -1; return false; }
  return true;
}
bool V4L2Capture::start(const CaptureConfig& c, uint64_t generation) {
  last_error_.clear(); warning_.clear();
  last_errno_ = 0;
  if (fd_ < 0 || streaming_ || buffers_allocated_ || stop_requested_.load()) {
    last_error_ = "START_STATE_CONFLICT"; return false;
  }
  auto fail = [&](std::string why, int cause = 0) { stop(); last_error_ = std::move(why); last_errno_ = cause; return false; };
  auto system_fail = [&](const char* op) { const int cause = errno; return fail(error(op), cause); };
  if (!generation || !ControlRouter::valid_config(c)) return fail("INVALID_CAPTURE_CONFIG");
  v4l2_capability cap{};
  if (control(VIDIOC_QUERYCAP, &cap) < 0) return system_fail("QUERYCAP");
  const auto caps = cap.capabilities & V4L2_CAP_DEVICE_CAPS ? cap.device_caps : cap.capabilities;
  if (!(caps & V4L2_CAP_VIDEO_CAPTURE)) return fail("UNSUPPORTED_SINGLE_PLANE_CAPTURE");
  if (!(caps & V4L2_CAP_STREAMING)) return fail("UNSUPPORTED_STREAMING");
  v4l2_format fmt{}; fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  fmt.fmt.pix.width = c.width; fmt.fmt.pix.height = c.height;
  fmt.fmt.pix.pixelformat = c.fourcc; fmt.fmt.pix.field = V4L2_FIELD_ANY;
  if (control(VIDIOC_S_FMT, &fmt) < 0) return system_fail("S_FMT");
  fmt = {}; fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  if (control(VIDIOC_G_FMT, &fmt) < 0) return system_fail("G_FMT");
  const auto& f = fmt.fmt.pix;
  if (!f.width || !f.height || f.width > 8192 || f.height > 8192 || !f.sizeimage || f.sizeimage > max_image)
    return fail("INVALID_NEGOTIATED_SIZE");
  if (f.pixelformat != c.fourcc) return fail("UNSUPPORTED_CHANGED_FOURCC");
  if (f.pixelformat == V4L2_PIX_FMT_YUYV && (f.width % 2 || uint64_t(f.bytesperline) < uint64_t(f.width) * 2 ||
      uint64_t(f.sizeimage) < uint64_t(f.bytesperline) * f.height)) return fail("INVALID_YUYV_LAYOUT");
  format_ = {}; format_.width = f.width; format_.height = f.height; format_.fourcc = f.pixelformat;
  format_.stride = f.bytesperline; format_.sizeimage = f.sizeimage;
  format_.pool_bytes = uint64_t(f.sizeimage) * c.pool_slots;
  if (format_.pool_bytes + uint64_t(f.sizeimage) * c.driver_buffers > c.memory_budget_bytes)
    return fail("NEGOTIATED_MEMORY_BUDGET");
  v4l2_streamparm parm{}; parm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  parm.parm.capture.timeperframe.numerator = c.fps_den;
  parm.parm.capture.timeperframe.denominator = c.fps_num;
  if (control(VIDIOC_S_PARM, &parm) < 0) {
    if (errno != EINVAL && errno != ENOTTY) return system_fail("S_PARM");
    warning_ = "FRAME_INTERVAL_REQUEST_UNSUPPORTED";
  }
  parm = {}; parm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  if (control(VIDIOC_G_PARM, &parm) == 0) {
    format_.interval_num = parm.parm.capture.timeperframe.numerator;
    format_.interval_den = parm.parm.capture.timeperframe.denominator;
  } else warning_ += warning_.empty() ? "FRAME_INTERVAL_UNKNOWN" : ";FRAME_INTERVAL_UNKNOWN";
  v4l2_requestbuffers req{}; req.count = c.driver_buffers;
  req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE; req.memory = V4L2_MEMORY_MMAP;
  if (control(VIDIOC_REQBUFS, &req) < 0) return system_fail("REQBUFS");
  buffers_allocated_ = req.count > 0;
  if (req.count < 2 || req.count > 32) return fail("INVALID_GRANTED_BUFFER_COUNT");
  format_.driver_buffers = req.count;
  if (format_.pool_bytes + uint64_t(f.sizeimage) * req.count > c.memory_budget_bytes)
    return fail("GRANTED_MEMORY_BUDGET");
  try { mappings_.resize(req.count); } catch (...) { return fail("MAPPING_TABLE_ALLOCATION"); }
  for (uint32_t i = 0; i < req.count; ++i) {
    v4l2_buffer b{}; b.type = req.type; b.memory = req.memory; b.index = i;
    if (control(VIDIOC_QUERYBUF, &b) < 0) return system_fail("QUERYBUF");
    if (b.length < f.sizeimage || b.length > max_image) return fail("INVALID_MAPPING_LENGTH");
    if (format_.mapped_bytes + b.length + format_.pool_bytes > c.memory_budget_bytes)
      return fail("MAPPED_MEMORY_BUDGET");
    auto& mapping = mappings_[i]; mapping.length = b.length;
    mapping.addr = io_->map_buffer(fd_, b.length, b.m.offset);
    if (mapping.addr == MAP_FAILED) { mapping.addr = nullptr; return system_fail("MMAP"); }
    mapping.mapped = true;
    if (!mapping.addr) return fail("MMAP_NULL_ADDRESS");
    format_.mapped_bytes += b.length;
  }
  for (uint32_t i = 0; i < req.count; ++i) {
    v4l2_buffer b{}; b.type = req.type; b.memory = req.memory; b.index = i;
    if (control(VIDIOC_QBUF, &b) < 0) return system_fail("INITIAL_QBUF");
  }
  auto type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  if (control(VIDIOC_STREAMON, &type) < 0) return system_fail("STREAMON");
  streaming_ = true; generation_ = generation; frame_id_ = 0; last_sequence_.reset(); counters_ = {};
  return true;
}
void V4L2Capture::account_sequence(uint32_t sequence) {
  if (last_sequence_) {
    const uint32_t step = sequence - *last_sequence_; // includes uint32 wrap
    if (!step) ++counters_.sequence_duplicates;
    else if (step < 0x80000000u) counters_.sequence_gaps += step - 1;
    else ++counters_.sequence_discontinuities;
  }
  last_sequence_ = sequence;
}
V4L2Capture::Read V4L2Capture::read(Frame* frame, int ms) {
  if (frame) frame->valid = false;
  auto fault = [&](std::string why) { last_error_ = std::move(why); ++counters_.faults; return Read::Fault; };
  if (!streaming_) return fault("READ_NOT_STREAMING");
  if (ms < 1 || ms > 1000) return fault("INVALID_POLL_BUDGET");
  const auto deadline = io_->monotonic_ns() + uint64_t(ms) * 1000000;
  v4l2_buffer b{};
  for (;;) {
    if (stop_requested_.load()) return Read::Stopped;
    const auto now = io_->monotonic_ns();
    if (now >= deadline) { ++counters_.timeouts; return Read::Timeout; }
    auto ready = io_->wait_frame_or_stop(fd_, stop_fd_, int((deadline - now + 999999) / 1000000));
    if (ready == V4L2Io::Wait::Stopped) return Read::Stopped;
    if (ready == V4L2Io::Wait::Timeout) { ++counters_.timeouts; return Read::Timeout; }
    if (ready == V4L2Io::Wait::Fault) return fault(error("POLL"));
    if (ready == V4L2Io::Wait::Retry) continue;
    b = {}; b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE; b.memory = V4L2_MEMORY_MMAP;
    if (control(VIDIOC_DQBUF, &b) == 0) break;
    if (errno == EAGAIN || errno == EINTR) continue;
    return fault(error("DQBUF"));
  }
  const auto host_dequeue = io_->monotonic_ns();
  ++counters_.dequeued; account_sequence(b.sequence);
  if (b.index >= mappings_.size()) return fault("DQBUF_INVALID_INDEX");
  const auto requeue = [&] {
    v4l2_buffer q{}; q.type = b.type; q.memory = b.memory; q.index = b.index;
    if (control(VIDIOC_QBUF, &q) < 0) { ++counters_.qbuf_errors; return false; }
    return true;
  };
  const bool truncated = format_.fourcc == V4L2_PIX_FMT_YUYV &&
      b.bytesused < uint64_t(format_.height - 1) * format_.stride + uint64_t(format_.width) * 2;
  if (!b.bytesused || b.bytesused > mappings_[b.index].length || b.bytesused > format_.sizeimage ||
      truncated || (b.flags & V4L2_BUF_FLAG_ERROR)) {
    ++counters_.invalid_payloads;
    if (!requeue()) return fault(error("QBUF_REJECTED_PAYLOAD"));
    return Read::InvalidPayload;
  }
  if (!frame) {
    ++counters_.no_slot;
    if (!requeue()) return fault(error("QBUF_NO_SLOT"));
    return Read::NoSlot;
  }
  if (frame->data.capacity() < format_.sizeimage) {
    if (!requeue()) return fault(error("QBUF_SMALL_SLOT"));
    return fault("SLOT_CAPACITY_NOT_RESERVED");
  }
  frame->width = format_.width; frame->height = format_.height; frame->pixfmt_fourcc = format_.fourcc;
  frame->stride = format_.stride; frame->bytesused = b.bytesused; frame->session_generation = generation_;
  frame->host_dequeue_mono_ns = host_dequeue; frame->publish_mono_ns = 0;
  frame->driver_sequence = b.sequence; frame->driver_flags = b.flags; frame->frame_id = frame_id_++;
  frame->driver_timestamp_ns.reset(); frame->driver_timestamp_clock = FrameMeta::TimestampClock::Unknown;
  frame->driver_timestamp_source = FrameMeta::TimestampSource::Unknown;
  if (b.timestamp.tv_sec >= 0 && b.timestamp.tv_usec >= 0 && b.timestamp.tv_usec < 1000000 &&
      uint64_t(b.timestamp.tv_sec) <= (UINT64_MAX - 999999000ULL) / 1000000000ULL) {
    frame->driver_timestamp_ns = uint64_t(b.timestamp.tv_sec) * 1000000000 + uint64_t(b.timestamp.tv_usec) * 1000;
    if ((b.flags & V4L2_BUF_FLAG_TIMESTAMP_MASK) == V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC)
      frame->driver_timestamp_clock = FrameMeta::TimestampClock::Monotonic;
    const auto source = b.flags & V4L2_BUF_FLAG_TSTAMP_SRC_MASK;
    if (source == V4L2_BUF_FLAG_TSTAMP_SRC_EOF) frame->driver_timestamp_source = FrameMeta::TimestampSource::EndOfFrame;
    else if (source == V4L2_BUF_FLAG_TSTAMP_SRC_SOE) frame->driver_timestamp_source = FrameMeta::TimestampSource::StartOfExposure;
  }
  try {
    const auto* p = static_cast<const uint8_t*>(mappings_[b.index].addr);
    frame->data.assign(p, p + b.bytesused);
  } catch (...) { requeue(); return fault("SLOT_COPY_FAILED"); }
  if (!requeue()) return fault(error("QBUF"));
  frame->valid = true; ++counters_.frames; last_error_.clear(); return Read::Frame;
}
void V4L2Capture::request_stop() noexcept {
  stop_requested_.store(true);
  if (stop_fd_ >= 0) io_->signal_stop(stop_fd_);
}
void V4L2Capture::stop() {
  if (fd_ >= 0 && streaming_) {
    auto type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (control(VIDIOC_STREAMOFF, &type) < 0) {
      last_error_ = error("STREAMOFF");
      // Closing the sole device owner cancels its queue before mappings vanish.
      io_->close_device(fd_); fd_ = -1;
    }
  }
  streaming_ = false;
  for (auto& mapping : mappings_) if (mapping.mapped) {
    if (io_->unmap_buffer(mapping.addr, mapping.length) < 0) last_error_ = error("MUNMAP");
    else mapping.mapped = false;
  }
  mappings_.erase(std::remove_if(mappings_.begin(), mappings_.end(), [](const Mapping& m) { return !m.mapped; }), mappings_.end());
  if (fd_ >= 0 && buffers_allocated_ && mappings_.empty()) {
    v4l2_requestbuffers r{}; r.type = V4L2_BUF_TYPE_VIDEO_CAPTURE; r.memory = V4L2_MEMORY_MMAP;
    if (control(VIDIOC_REQBUFS, &r) < 0) last_error_ = error("REQBUFS_RELEASE");
  }
  buffers_allocated_ = false;
}
void V4L2Capture::close() {
  stop();
  if (fd_ >= 0) { io_->close_device(fd_); fd_ = -1; }
  if (stop_fd_ >= 0) { io_->close_device(stop_fd_); stop_fd_ = -1; }
}
}  // namespace edgevision
