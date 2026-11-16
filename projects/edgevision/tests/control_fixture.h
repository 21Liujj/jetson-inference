#pragma once
#include "edgevision/control_router.h"
#include <cassert>
#include <deque>
#include <functional>
#include <stdexcept>
#include <utility>
using namespace edgevision;
// Explicit CPU fixture. No device, driver, image, CUDA or model is opened.
struct Fixture final : CaptureBackend {
  unsigned starts = 0, stops = 0;
  bool reject = false, throw_start = false, throw_event = false;
  uint64_t last_generation = 0;
  std::function<void()> inspect;
  std::deque<CaptureEvent> events;
  BackendStartReply request_start(const CaptureConfig&, uint64_t generation) override {
    ++starts; last_generation = generation; if (inspect) inspect();
    if (throw_start) throw std::runtime_error("partial start fixture");
    return {!reject, reject ? "NO_DEVICE" : ""};
  }
  void request_stop(uint64_t generation) noexcept override { assert(generation == last_generation); ++stops; if (inspect) inspect(); }
  std::optional<CaptureEvent> poll_event() override {
    if (throw_event) { throw_event = false; throw std::runtime_error("poll fixture"); }
    if (inspect) inspect(); if (events.empty()) return std::nullopt;
    auto event = events.front(); events.pop_front(); return event;
  }
  void first(uint64_t generation, bool stream_on = true) {
    CaptureEvent e; e.kind = CaptureEvent::Kind::FirstFrame; e.generation = generation; e.stream_on = stream_on;
    e.frame.session_generation = generation; e.frame.frame_id = 47; e.frame.width = 640; e.frame.height = 360;
    e.frame.stride = 1280; e.frame.bytesused = 460800; e.frame.host_dequeue_mono_ns = 1;
    e.frame.pixfmt_fourcc = make_fourcc('Y','U','Y','V'); events.push_back(e);
  }
  void stopped(uint64_t generation) { CaptureEvent e; e.kind = CaptureEvent::Kind::Stopped; e.generation = generation; events.push_back(e); }
};
static ControlRequest start(std::string id, uint64_t generation = 0, uint64_t deadline = 1000) {
  CaptureConfig c; c.device = "/dev/v4l/by-id/explicit-cpu-fixture";
  return {std::move(id), generation, deadline, TaskCommand::StartCapture, c};
}
static ControlRequest stop(std::string id, uint64_t generation) {
  return {std::move(id), generation, 1000, TaskCommand::StopCapture, std::nullopt};
}
