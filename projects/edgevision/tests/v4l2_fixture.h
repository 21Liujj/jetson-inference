#pragma once
#include "edgevision/v4l2_capture.h"
#include <linux/videodev2.h>
#include <poll.h>
#include <sys/eventfd.h>
#include <sys/mman.h>
#include <unistd.h>
#include <cassert>
#include <cerrno>
using namespace edgevision;
// Explicit syscall fixture. Real eventfd/poll is exercised, but no camera opens.
struct Fixture final : V4L2Io {
  std::atomic<unsigned> streamoffs{0}; bool fail_open = false;
  unsigned opens = 0, closes = 0, event_closes = 0, maps = 0, unmaps = 0, releases = 0, qbufs = 0;
  uint32_t width = 640, height = 360, size = 1024, stride = 0, maplen = 1024, granted = 4, bytes = 16;
  uint32_t pixfmt = V4L2_PIX_FMT_MJPEG, index = 0, sequence = 0;
  uint32_t flags = V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC | V4L2_BUF_FLAG_TSTAMP_SRC_SOE;
  int bad_map = -1; bool no_frames = false, fail_qbuf = false, bad_time = false, multi_only = false;
  unsigned long fail_request = 0;
  bool fail_once = false;
  std::atomic<bool> waiting{false};
  uint64_t now = 1000000000;
  std::vector<std::vector<uint8_t>> memory = std::vector<std::vector<uint8_t>>(32, std::vector<uint8_t>(4096, 42));
  int open_device(const char*) override { ++opens; if (fail_open) { errno = ENOENT; return -1; } return 4242; }
  int close_device(int fd) override { if (fd == 4242) { ++closes; return 0; } ++event_closes; return ::close(fd); }
  int create_stop_event() override { return ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC); }
  void signal_stop(int fd) noexcept override { uint64_t one = 1; (void)::write(fd, &one, sizeof(one)); }
  int control(int fd, unsigned long op, void* p) override {
    assert(fd == 4242);
    if (op == fail_request) { if (fail_once) fail_request = 0; errno = EIO; return -1; }
    if (op == VIDIOC_QUERYCAP) {
      auto& c = *static_cast<v4l2_capability*>(p); c.capabilities = V4L2_CAP_DEVICE_CAPS;
      c.device_caps = (multi_only ? V4L2_CAP_VIDEO_CAPTURE_MPLANE : V4L2_CAP_VIDEO_CAPTURE) | V4L2_CAP_STREAMING;
    } else if (op == VIDIOC_S_FMT || op == VIDIOC_G_FMT) {
      auto& f = static_cast<v4l2_format*>(p)->fmt.pix;
      f.width = width; f.height = height; f.pixelformat = pixfmt; f.sizeimage = size; f.bytesperline = stride;
    } else if (op == VIDIOC_G_PARM || op == VIDIOC_S_PARM) {
      auto& t = static_cast<v4l2_streamparm*>(p)->parm.capture.timeperframe; t.numerator = 1; t.denominator = 30;
    } else if (op == VIDIOC_REQBUFS) {
      auto& r = *static_cast<v4l2_requestbuffers*>(p); if (!r.count) ++releases; else r.count = granted;
    } else if (op == VIDIOC_QUERYBUF) {
      auto& b = *static_cast<v4l2_buffer*>(p); b.length = maplen; b.m.offset = b.index * 4096;
    } else if (op == VIDIOC_QBUF) { ++qbufs; if (fail_qbuf) { errno = EIO; return -1; } }
    else if (op == VIDIOC_DQBUF) {
      auto& b = *static_cast<v4l2_buffer*>(p); b.index = index; b.bytesused = bytes; b.flags = flags; b.sequence = sequence++;
      b.timestamp.tv_sec = 1; b.timestamp.tv_usec = bad_time ? 1000000 : 500;
    } else { assert(op == VIDIOC_STREAMON || op == VIDIOC_STREAMOFF); if (op == VIDIOC_STREAMOFF) ++streamoffs; }
    return 0;
  }
  void* map_buffer(int, std::size_t, uint64_t offset) override {
    unsigned i = maps++; if (int(i) == bad_map) { errno = ENOMEM; return MAP_FAILED; }
    return memory.at(offset / 4096).data();
  }
  int unmap_buffer(void*, std::size_t) override { ++unmaps; return 0; }
  Wait wait_frame_or_stop(int, int stop, int ms) override {
    if (!no_frames) return Wait::Frame;
    waiting.store(true); pollfd p{stop, POLLIN, 0}; int n = ::poll(&p, 1, ms);
    return n > 0 ? Wait::Stopped : n == 0 ? Wait::Timeout : Wait::Fault;
  }
  uint64_t monotonic_ns() override { now += 1000; return now; }
};
