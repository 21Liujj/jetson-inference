#include "edgevision/upstream_detector.h"
#include "edgevision/image_layout.h"
#include "edgevision/upstream_yolo.h"
#include "edgevision/yolo_preprocess.h"
#include <jetson-inference/detectNet.h>
#include <jetson-utils/cudaColorspace.h>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <thread>

namespace edgevision {
namespace {
uint64_t now() { return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
[[noreturn]] void gpu_isolation(cudaError_t code) noexcept {
  // A failed completion barrier cannot authorize RAII reuse of in-flight memory.
  // Terminate this worker process; the supervisor records the nonzero exit.
  std::fprintf(stderr, "GPU_WORKER_FATAL: completion unknown: %s; process isolation\n", cudaGetErrorString(code));
  std::fflush(nullptr); std::_Exit(70);
}
void completed_barrier() noexcept { const auto e = cudaDeviceSynchronize(); if (e != cudaSuccess) gpu_isolation(e); }
void check(cudaError_t e, const char* operation) {
  if (e != cudaSuccess) throw std::runtime_error(std::string(operation) + ": " + cudaGetErrorString(e));
}
class CudaFence final : public CompletionFence {
 public:
  CudaFence() { check(cudaEventCreateWithFlags(&event_, cudaEventDisableTiming), "CUDA_EVENT_CREATE"); }
  ~CudaFence() override { wait(); if (event_) cudaEventDestroy(event_); }
  void record() { check(cudaEventRecord(event_, 0), "CUDA_EVENT_RECORD"); recorded_ = true; }
  bool ready() const noexcept override { return recorded_ && cudaEventQuery(event_) == cudaSuccess; }
  void wait() noexcept override {
    if (!recorded_) { completed_barrier(); return; }
    const auto e = cudaEventSynchronize(event_); if (e != cudaSuccess) gpu_isolation(e);
  }
 private:
  cudaEvent_t event_ = nullptr;
  bool recorded_ = false;
};
struct MappedBuffer {
  void* cpu = nullptr; void* gpu = nullptr; std::size_t bytes = 0;
  ~MappedBuffer() { if (cpu) cudaFreeHost(cpu); }
  void reserve(std::size_t n) {
    if (n <= bytes) return;
    // Only called between completed inferences; bounded by packed_yuyv_bytes.
    void* next_cpu = nullptr; void* next_gpu = nullptr;
    check(cudaHostAlloc(&next_cpu, n, cudaHostAllocMapped), "CUDA_HOST_ALLOC");
    const auto e = cudaHostGetDevicePointer(&next_gpu, next_cpu, 0);
    if (e != cudaSuccess) { cudaFreeHost(next_cpu); check(e, "CUDA_HOST_DEVICE_POINTER"); }
    if (cpu) {
      const auto release = cudaFreeHost(cpu);
      if (release != cudaSuccess) { cudaFreeHost(next_cpu); check(release, "CUDA_FREE_HOST"); }
    }
    cpu = next_cpu; gpu = next_gpu; bytes = n;
  }
};
struct DeviceBuffer {
  void* pointer = nullptr; std::size_t bytes = 0;
  ~DeviceBuffer() { if (pointer) cudaFree(pointer); }
  void reserve(std::size_t n) {
    if (n <= bytes) return;
    void* next = nullptr; check(cudaMalloc(&next, n), "CUDA_RGB_ALLOC");
    if (pointer) {
      const auto release = cudaFree(pointer);
      if (release != cudaSuccess) { cudaFree(next); check(release, "CUDA_RGB_FREE"); }
    }
    pointer = next; bytes = n;
  }
};
}
struct UpstreamDetector::Impl {
  std::thread::id owner = std::this_thread::get_id();
  std::unique_ptr<detectNet> net;
  std::unique_ptr<UpstreamYolo> yolo;
  std::vector<std::string> labels;
  std::vector<uint8_t> cpu_rgb, tensor_bytes;
  std::vector<float> tensor;
  MappedBuffer raw;
  DeviceBuffer rgb;
  void require_owner() const {
    if (owner != std::this_thread::get_id()) throw std::logic_error("GPU_CONTEXT_OWNER_MISMATCH");
  }
  ~Impl() {
    if (owner != std::this_thread::get_id()) { std::fputs("GPU_OWNER_DESTRUCTION_MISMATCH\n", stderr); std::_Exit(70); }
    if (net || yolo) completed_barrier();
  }
};
UpstreamDetector::UpstreamDetector() : impl_(new Impl) {}
UpstreamDetector::~UpstreamDetector() = default;
void UpstreamDetector::initialize(const std::string& onnx, const std::string& labels, DetectorKind kind) {
  impl_->require_owner();
  if (impl_->net || impl_->yolo) throw std::logic_error("MODEL_ALREADY_INITIALIZED");
  if (!std::filesystem::is_regular_file(onnx) || !std::filesystem::is_regular_file(labels))
    throw std::invalid_argument("MODEL_FILE_NOT_FOUND");
  if (kind==DetectorKind::YoloV8Coco) {
    std::ifstream file(labels); std::string line;
    while (std::getline(file,line)) {
      if (!line.empty() && line.back()=='\r') line.pop_back();
      if (line.empty() || line.size()>128 || impl_->labels.size()==80 || line.find_first_of("\t\n")!=std::string::npos)
        throw std::invalid_argument("INVALID_COCO_LABELS");
      impl_->labels.push_back(line);
    }
    if (!file.eof() || impl_->labels.size()!=80) throw std::invalid_argument("EXPECTED_80_COCO_LABELS");
    impl_->yolo=std::make_unique<UpstreamYolo>(); impl_->yolo->initialize(onnx);
    return;
  }
  if (kind!=DetectorKind::SsdFruit) throw std::invalid_argument("UNSUPPORTED_MODEL_KIND");
  impl_->net.reset(detectNet::Create(nullptr, onnx.c_str(), 0.0f, labels.c_str(), 0.5f,
                                  "input_0", "scores", "boxes", 1, TYPE_FP16, DEVICE_GPU, true));
  if (!impl_->net) throw std::runtime_error("UPSTREAM_MODEL_LOAD_FAILED");
  // ProcessNetwork(true) uses executeV2/default stream in the frozen upstream.
  // Keep this serial baseline on stream 0. A nonblocking stream would require
  // changing and independently verifying that upstream execution contract.
  if (impl_->net->GetStream() != 0) throw std::runtime_error("UNSUPPORTED_UPSTREAM_STREAM");
  const auto i = impl_->net->GetInputDims(); const auto s = impl_->net->GetOutputDims(0); const auto b = impl_->net->GetOutputDims(1);
  if (i.d[0] != 3 || i.d[1] != 300 || i.d[2] != 300 || s.d[0] != 3000 || s.d[1] != 9 || s.d[2] != 1 ||
      b.d[0] != 3000 || b.d[1] != 4 || b.d[2] != 1 || impl_->net->GetNumClasses() != 9 ||
      impl_->net->GetInputSize() != 3 * 300 * 300 * sizeof(float) ||
      impl_->net->GetOutputSize(0) != 3000 * 9 * sizeof(float) || impl_->net->GetOutputSize(1) != 3000 * 4 * sizeof(float))
    throw std::runtime_error("UNSUPPORTED_SSD_BASELINE_CONTRACT");
}
DetectionResult UpstreamDetector::infer(FrameLease frame, const SubmitObserver& gpu_submitted) {
  impl_->require_owner();
  if ((!impl_->net && !impl_->yolo) || !frame.valid() || frame.state() != SlotState::Ready) throw std::logic_error("MODEL_OR_FRAME_NOT_READY");
  const auto bytes = packed_yuyv_bytes(*frame, frame->data.size());
  impl_->raw.reserve(bytes); impl_->rgb.reserve(std::size_t(frame->width) * frame->height * 3);
  pack_yuyv(*frame, impl_->raw.cpu, impl_->raw.bytes);
  DetectionResult result; result.frame = *frame;
  auto fence = std::make_shared<CudaFence>();
  if (!frame.mark_inflight(fence)) throw std::logic_error("FRAME_INFLIGHT_TRANSITION");
  try {
    check(cudaConvertColor(impl_->raw.gpu, IMAGE_YUYV, impl_->rgb.pointer, IMAGE_RGB8,
                           frame->width, frame->height, cudaStream_t(0)), "UPSTREAM_YUYV_TO_RGB");
    if (gpu_submitted) gpu_submitted(frame);
    if (impl_->yolo) {
      // M3 serial correctness baseline. M5 will profile and remove this GPU->CPU
      // round trip with a numerically checked fused preprocess kernel.
      impl_->cpu_rgb.resize(std::size_t(frame->width)*frame->height*3);
      check(cudaMemcpy(impl_->cpu_rgb.data(),impl_->rgb.pointer,impl_->cpu_rgb.size(),cudaMemcpyDeviceToHost),"YOLO_RGB_DOWNLOAD");
      const auto geometry=yolo_letterbox_rgb(impl_->cpu_rgb.data(),impl_->cpu_rgb.size(),frame->width,frame->height,
                                             std::size_t(frame->width)*3,impl_->tensor_bytes);
      impl_->tensor.resize(impl_->tensor_bytes.size());
      for (std::size_t n=0;n<impl_->tensor.size();++n) impl_->tensor[n]=float(impl_->tensor_bytes[n])/255.f;
      const auto raw=impl_->yolo->infer(impl_->tensor.data(),impl_->tensor.size());
      for (const auto& b : decode_yolov8(raw.data(),raw.size(),geometry))
        result.boxes.push_back({b.class_id,b.score,b.left,b.top,b.right,b.bottom,impl_->labels[b.class_id]});
      completed_barrier(); fence->record(); fence->wait();
    } else {
      detectNet::Detection* boxes = nullptr;
      const auto count = impl_->net->Detect(impl_->rgb.pointer, frame->width, frame->height, IMAGE_RGB8, &boxes, detectNet::OVERLAY_NONE);
    // Includes conversion, upstream preprocessing and any downstream GPU work.
    // Coarse synchronization is intentional for this correctness baseline.
    completed_barrier(); fence->record(); fence->wait();
    if (count < 0 || uint32_t(count) > impl_->net->GetMaxDetections()) throw std::runtime_error("UPSTREAM_DETECTION_FAILED");
    result.boxes.reserve(count);
    for (int n = 0; n < count; ++n) {
      const auto& b = boxes[n];
      if (b.ClassID >= impl_->net->GetNumClasses() || !std::isfinite(b.Confidence) || !std::isfinite(b.Left) ||
          !std::isfinite(b.Top) || !std::isfinite(b.Right) || !std::isfinite(b.Bottom) || b.Confidence < 0 || b.Confidence > 1 ||
          b.Right < b.Left || b.Bottom < b.Top) throw std::runtime_error("INVALID_DETECTION_RESULT");
      result.boxes.push_back({int(b.ClassID), b.Confidence, b.Left, b.Top, b.Right, b.Bottom, impl_->net->GetClassDesc(b.ClassID)});
    }
    }
    result.completed_mono_ns = now();
    if (!frame.complete_if_ready()) throw std::logic_error("CUDA_FRAME_COMPLETION_NOT_OBSERVED");
    return result;
  } catch (...) {
    // Never release a lease based on an exception, cancellation or timer alone.
    completed_barrier(); throw;
  }
}
} // namespace edgevision
