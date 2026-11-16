#pragma once
#include <cstddef>
#include <memory>
#include <string>
#include <vector>

namespace edgevision {
// Static YOLO adapter built on the repaired upstream tensorNet. Serial M3
// correctness baseline: no independent engine loader or async overlap claim.
class UpstreamYolo {
 public:
  UpstreamYolo();
  ~UpstreamYolo();
  UpstreamYolo(const UpstreamYolo&) = delete;
  UpstreamYolo& operator=(const UpstreamYolo&) = delete;
  void initialize(const std::string& onnx, bool fp16=true);
  std::vector<float> infer(const float* nchw, std::size_t elements);
 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
} // namespace edgevision
