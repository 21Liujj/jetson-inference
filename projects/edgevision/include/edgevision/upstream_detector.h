#pragma once
#include "edgevision/frame_lease.h"
#include <memory>
#include <functional>
#include <string>
#include <vector>

namespace edgevision {
struct DetectionBox {
  int class_id = 0;
  float confidence = 0, left = 0, top = 0, right = 0, bottom = 0;
  std::string label;
};
struct DetectionResult {
  FrameMeta frame;
  uint64_t completed_mono_ns = 0;
  std::vector<DetectionBox> boxes; // empty is a successful no-detection result
};
// Serial M2 input adapter. Construct, initialize, infer and destroy on the one
// GPU owner thread. Uses repaired upstream DetectNet, not a second TRT loader.
// This baseline is NOT the M5 asynchronous execution layer.
class UpstreamDetector {
 public:
  UpstreamDetector();
  ~UpstreamDetector();
  UpstreamDetector(const UpstreamDetector&) = delete;
  UpstreamDetector& operator=(const UpstreamDetector&) = delete;
  void initialize(const std::string& onnx, const std::string& labels);
  using SubmitObserver = std::function<void(const FrameLease&)>;
  DetectionResult infer(FrameLease, const SubmitObserver& gpu_submitted = {});
 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
} // namespace edgevision
