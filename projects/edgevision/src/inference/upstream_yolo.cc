#include "edgevision/upstream_yolo.h"
#include <jetson-inference/tensorNet.h>
#include <cuda_runtime_api.h>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <stdexcept>
#include <thread>

namespace edgevision {
namespace {
void check(cudaError_t e,const char* op) {
  if(e != cudaSuccess) throw std::runtime_error(std::string(op)+": "+cudaGetErrorString(e));
}
void completed() noexcept {
  const auto e=cudaDeviceSynchronize();
  if(e != cudaSuccess) {
    std::fprintf(stderr,"GPU_WORKER_FATAL: YOLO completion unknown: %s\n",cudaGetErrorString(e));
    std::fflush(nullptr); std::_Exit(70);
  }
}
class YoloNet final : public tensorNet {
 public:
  bool initialize(const std::string& path,bool fp16) {
    if(!LoadNetwork(nullptr,path.c_str(),nullptr,"images","output0",1,
                    fp16 ? TYPE_FP16 : TYPE_FP32,DEVICE_GPU,true)) return false;
    const auto i=GetInputDims(), o=GetOutputDims();
    // Upstream strips the explicit batch, padding unused dimensions with 1.
    if(GetInputLayers()!=1 || GetOutputLayers()!=1 || GetStream()!=0 ||
       i.d[0]!=3 || i.d[1]!=640 || i.d[2]!=640 ||
       o.d[0]!=84 || o.d[1]!=8400 || o.d[2]!=1 ||
       GetInputSize()!=3*640*640*sizeof(float) || GetOutputSize()!=84*8400*sizeof(float))
      throw std::runtime_error("UNSUPPORTED_YOLO_STATIC_CONTRACT");
    return true;
  }
  std::vector<float> infer(const float* x) {
    check(cudaMemcpy(GetInputPtr(),x,GetInputSize(),cudaMemcpyHostToDevice),"YOLO_INPUT_COPY");
    if(!ProcessNetwork(true)) { completed(); throw std::runtime_error("YOLO_EXECUTION_FAILED"); }
    completed();
    std::vector<float> output(84*8400);
    check(cudaMemcpy(output.data(),GetOutputPtr(),GetOutputSize(),cudaMemcpyDeviceToHost),"YOLO_OUTPUT_COPY");
    return output;
  }
};
}
struct UpstreamYolo::Impl {
  std::thread::id owner=std::this_thread::get_id();
  std::unique_ptr<YoloNet> net;
  void require_owner() const {
    if(owner!=std::this_thread::get_id()) throw std::logic_error("GPU_CONTEXT_OWNER_MISMATCH");
  }
  ~Impl() {
    if(owner!=std::this_thread::get_id()) { std::fputs("GPU_OWNER_DESTRUCTION_MISMATCH\n",stderr); std::_Exit(70); }
    if(net) completed();
  }
};
UpstreamYolo::UpstreamYolo():impl_(new Impl) {}
UpstreamYolo::~UpstreamYolo()=default;
void UpstreamYolo::initialize(const std::string& onnx,bool fp16) {
  impl_->require_owner();
  if(impl_->net) throw std::logic_error("MODEL_ALREADY_INITIALIZED");
  if(!std::filesystem::is_regular_file(onnx)) throw std::invalid_argument("MODEL_FILE_NOT_FOUND");
  auto next=std::make_unique<YoloNet>();
  if(!next->initialize(onnx,fp16)) throw std::runtime_error("UPSTREAM_YOLO_LOAD_FAILED");
  impl_->net=std::move(next);
}
std::vector<float> UpstreamYolo::infer(const float* x,std::size_t n) {
  impl_->require_owner();
  if(!impl_->net || !x || n!=3*640*640) throw std::invalid_argument("INVALID_YOLO_INPUT");
  for(std::size_t i=0;i<n;++i) if(!std::isfinite(x[i]) || x[i]<0 || x[i]>1)
    throw std::invalid_argument("INVALID_YOLO_NORMALIZED_INPUT");
  try { return impl_->net->infer(x); }
  catch(...) { completed(); throw; }
}
} // namespace edgevision
