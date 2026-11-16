#include "edgevision/upstream_yolo.h"
#include "edgevision/nvtx_scope.h"
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
  ~YoloNet() override { if(queued_)wait(); if(done_)cudaEventDestroy(done_); }
  bool initialize(const std::string& path,bool fp16) {
    if(!LoadNetwork(nullptr,path.c_str(),nullptr,"images","output0",1,
                    fp16 ? TYPE_FP16 : TYPE_FP32,DEVICE_GPU,true)) return false;
    if(mEngine->getTensorDataType("images")!=nvinfer1::DataType::kFLOAT ||
       mEngine->getTensorDataType("output0")!=nvinfer1::DataType::kFLOAT)
      throw std::runtime_error("YOLO_REQUIRES_FP32_IO");
    const auto i=GetInputDims(), o=GetOutputDims();
    // Upstream strips the explicit batch, padding unused dimensions with 1.
    if(GetInputLayers()!=1 || GetOutputLayers()!=1 || GetStream()!=0 ||
       i.d[0]!=3 || i.d[1]!=640 || i.d[2]!=640 ||
       o.d[0]!=84 || o.d[1]!=8400 || o.d[2]!=1 ||
       GetInputSize()!=3*640*640*sizeof(float) || GetOutputSize()!=84*8400*sizeof(float))
      throw std::runtime_error("UNSUPPORTED_YOLO_STATIC_CONTRACT");
    check(cudaEventCreateWithFlags(&done_,cudaEventDisableTiming),"YOLO_COMPLETION_EVENT");
    return true;
  }
  bool context_ready() {
    if(!queued_)return true;
    const auto e=cudaEventQuery(done_);
    if(e==cudaErrorNotReady)return false;
    check(e,"YOLO_CONTEXT_COMPLETION_QUERY"); queued_=false;return true;
  }
  void wait() {
    if(queued_) {const auto e=cudaEventSynchronize(done_);if(e!=cudaSuccess){completed();check(e,"YOLO_CONTEXT_WAIT");}queued_=false;}
  }
  void enqueue_device(const float* input,float* output,cudaStream_t stream) {
    if(!input || !output || !stream || !context_ready())throw std::logic_error("YOLO_CONTEXT_OR_IO_NOT_AVAILABLE");
    if(!mContext->setTensorAddress("images",const_cast<float*>(input)) || !mContext->setTensorAddress("output0",output))
      throw std::runtime_error("YOLO_NAMED_IO_BIND_FAILED");
    SetStream(stream);
    NvtxScope scope("inference/enqueueV3");
    // Mark before enqueue: a failed API can still require a physical barrier.
    queued_=true;
    if(!ProcessNetwork(false)) {completed();queued_=false;throw std::runtime_error("YOLO_ASYNC_ENQUEUE_FAILED");}
    const auto e=cudaEventRecord(done_,stream);
    if(e!=cudaSuccess) {completed();queued_=false;check(e,"YOLO_CONTEXT_EVENT_RECORD");}
  }
  std::vector<float> infer(const float* x) {
    if(!context_ready())throw std::logic_error("YOLO_CONTEXT_BUSY");
    SetStream(0);
    {NvtxScope scope("copy/P0-normalized-upload");check(cudaMemcpy(GetInputPtr(),x,GetInputSize(),cudaMemcpyHostToDevice),"YOLO_INPUT_COPY");}
    {NvtxScope scope("inference/P0-executeV2");if(!ProcessNetwork(true)) { completed(); throw std::runtime_error("YOLO_EXECUTION_FAILED"); }}
    completed();
    std::vector<float> output(84*8400);
    {NvtxScope scope("copy/P0-output-download");check(cudaMemcpy(output.data(),GetOutputPtr(),GetOutputSize(),cudaMemcpyDeviceToHost),"YOLO_OUTPUT_COPY");}
    return output;
  }
 private:
  cudaEvent_t done_=nullptr;
  bool queued_=false;
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
bool UpstreamYolo::context_ready() {impl_->require_owner();if(!impl_->net)throw std::logic_error("MODEL_NOT_INITIALIZED");return impl_->net->context_ready();}
void UpstreamYolo::enqueue_device(const float* input,float* output,cudaStream_t stream) {
  impl_->require_owner();if(!impl_->net)throw std::logic_error("MODEL_NOT_INITIALIZED");
  try {impl_->net->enqueue_device(input,output,stream);}catch(...){completed();throw;}
}
void UpstreamYolo::wait() {impl_->require_owner();if(impl_->net)impl_->net->wait();}
} // namespace edgevision
