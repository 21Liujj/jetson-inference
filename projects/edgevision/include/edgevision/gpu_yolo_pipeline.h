#pragma once
#include "edgevision/cuda_letterbox.h"
#include <memory>
#include <string>

namespace edgevision {
enum class GpuInputPath { PinnedDevice, Mapped };
struct GpuStageTiming {
  float copy_ms=0,preprocess_ms=0,inference_ms=0,critical_path_gpu_ms=0;
  uint64_t tag=0,started_mono_ns=0,completed_mono_ns=0;
  LetterboxGeometry geometry{};
  bool valid=false;
};
// One owner thread and one inference context. Slots have independent I/O;
// the next context binding is allowed only after the preceding real GPU event.
class GpuYoloPipeline {
 public:
  GpuYoloPipeline(const std::string& onnx,unsigned slots=2,GpuInputPath path=GpuInputPath::PinnedDevice);
  ~GpuYoloPipeline();
  GpuYoloPipeline(const GpuYoloPipeline&)=delete;
  GpuYoloPipeline& operator=(const GpuYoloPipeline&)=delete;
  void prepare_host(unsigned slot,uint64_t tag,const uint8_t* rgb,std::size_t bytes,
                    unsigned width,unsigned height,std::size_t stride);
  void prepare_device(unsigned slot,uint64_t tag,const uint8_t* rgb,std::size_t bytes,
                      unsigned width,unsigned height,std::size_t stride,cudaStream_t producer);
  bool submit(unsigned slot);
  bool collect(unsigned slot,std::vector<float>& output,GpuStageTiming& timing);
  void cancel(unsigned slot); // invalidate only; never releases pending memory
  bool discard_prepared(unsigned slot);
  std::string capabilities_json() const;
 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
} // namespace edgevision
