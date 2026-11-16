#pragma once
#include "edgevision/yolo_postprocess.h"
#include <cuda_runtime_api.h>
#include <cstdint>

namespace edgevision {
struct CudaLetterboxPlan {
  LetterboxGeometry geometry;
  int resized_width=0, resized_height=0;
};
CudaLetterboxPlan cuda_letterbox_plan(unsigned width,unsigned height);
// Caller owns input/output until the stream's completion event. No allocation
// or synchronization inside this launch wrapper. RGB8 only (not YUYV/NV12).
cudaError_t launch_rgb_letterbox(const uint8_t* rgb,std::size_t bytes,unsigned width,unsigned height,
                                 std::size_t stride,float* chw,std::size_t elements,cudaStream_t stream);
} // namespace edgevision
