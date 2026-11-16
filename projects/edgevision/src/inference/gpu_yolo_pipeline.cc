#include "edgevision/gpu_yolo_pipeline.h"
#include "edgevision/upstream_yolo.h"
#include "edgevision/nvtx_scope.h"
#include <chrono>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <sstream>
#include <thread>

namespace edgevision {
namespace {
uint64_t mono(){return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();}
void check(cudaError_t e,const char* op){if(e!=cudaSuccess)throw std::runtime_error(std::string(op)+":"+cudaGetErrorString(e));}
void physical_barrier() noexcept {
  if(cudaDeviceSynchronize()!=cudaSuccess){std::fputs("GPU_PIPELINE_COMPLETION_UNKNOWN\n",stderr);std::_Exit(70);}
}
struct Stream {
  cudaStream_t value=nullptr;
  Stream(){check(cudaStreamCreateWithFlags(&value,cudaStreamNonBlocking),"PIPELINE_STREAM");}
  ~Stream(){if(value)cudaStreamDestroy(value);}
};
struct Event {
  cudaEvent_t value=nullptr;
  Event(){check(cudaEventCreate(&value),"PIPELINE_EVENT");}
  ~Event(){if(value)cudaEventDestroy(value);}
  void record(cudaStream_t stream){check(cudaEventRecord(value,stream),"PIPELINE_EVENT_RECORD");}
  bool ready(){const auto e=cudaEventQuery(value);if(e==cudaErrorNotReady)return false;check(e,"PIPELINE_EVENT_QUERY");return true;}
};
struct Device {
  void* value=nullptr;
  explicit Device(std::size_t n){check(cudaMalloc(&value,n),"PIPELINE_DEVICE_ALLOC");}
  ~Device(){if(value)cudaFree(value);}
};
struct Pinned {
  void* cpu=nullptr;void* gpu=nullptr;
  Pinned(std::size_t n,bool mapped){
    check(cudaHostAlloc(&cpu,n,mapped?cudaHostAllocMapped:cudaHostAllocDefault),"PIPELINE_PINNED_ALLOC");
    if(mapped){const auto e=cudaHostGetDevicePointer(&gpu,cpu,0);if(e!=cudaSuccess){cudaFreeHost(cpu);cpu=nullptr;check(e,"PIPELINE_MAPPED_ALIAS");}}
  }
  ~Pinned(){if(cpu)cudaFreeHost(cpu);}
};
constexpr std::size_t input_capacity=4*1024*1024,input_elements=3*640*640,output_elements=84*8400;
enum class State {Free,Prepared,InFlight};
struct Slot {
  Pinned host,output;
  std::unique_ptr<Device> rgb;
  Device tensor{input_elements*sizeof(float)};
  Event source_ready,copy_start,copy_end,pre_start,pre_end,net_start,done;
  State state=State::Free;GpuStageTiming timing;bool mapped=false,external=false,invalid=false;
  explicit Slot(GpuInputPath path):host(input_capacity,path==GpuInputPath::Mapped),output(output_elements*sizeof(float),true),mapped(path==GpuInputPath::Mapped){
    if(!mapped)rgb=std::make_unique<Device>(input_capacity);
  }
};
}
struct GpuYoloPipeline::Impl {
  const std::thread::id owner=std::this_thread::get_id();
  GpuInputPath path;
  Stream preprocess,inference;
  std::vector<std::unique_ptr<Slot>> slots;
  std::unique_ptr<UpstreamYolo> net; // destroyed before slot I/O storage
  cudaDeviceProp properties{};
  int host_register=0,driver_version=0,runtime_version=0;
  Impl(const std::string& onnx,unsigned count,GpuInputPath input_path):path(input_path){
    if(count<1 || count>3)throw std::invalid_argument("PIPELINE_SLOT_BUDGET");
    check(cudaGetDeviceProperties(&properties,0),"PIPELINE_PROPERTIES");
    check(cudaDeviceGetAttribute(&host_register,cudaDevAttrHostRegisterSupported,0),"HOST_REGISTER_CAPABILITY");
    check(cudaDriverGetVersion(&driver_version),"CUDA_DRIVER_VERSION");check(cudaRuntimeGetVersion(&runtime_version),"CUDA_RUNTIME_VERSION");
    if(!properties.canMapHostMemory)throw std::runtime_error("MAPPED_OUTPUT_UNSUPPORTED");
    for(unsigned i=0;i<count;++i)slots.push_back(std::make_unique<Slot>(path));
    net=std::make_unique<UpstreamYolo>();net->initialize(onnx);
  }
  ~Impl(){
    if(owner!=std::this_thread::get_id()){std::fputs("GPU_PIPELINE_WRONG_DESTRUCTION_OWNER\n",stderr);std::_Exit(70);}
    // Includes prepared-but-not-submitted copies/kernels and every network job.
    const auto a=cudaStreamSynchronize(preprocess.value),b=cudaStreamSynchronize(inference.value);
    if(a!=cudaSuccess || b!=cudaSuccess)physical_barrier();
    if(net)net->wait();
  }
  void require_owner()const{if(owner!=std::this_thread::get_id())throw std::logic_error("GPU_PIPELINE_OWNER_MISMATCH");}
  Slot& get(unsigned index){require_owner();if(index>=slots.size())throw std::out_of_range("PIPELINE_SLOT_INDEX");return *slots[index];}
  void start(Slot& s,uint64_t tag,std::size_t bytes,unsigned w,unsigned h,std::size_t stride){
    if(s.state!=State::Free)throw std::logic_error("PIPELINE_SLOT_STILL_OWNED");
    if(!w || !h || stride<std::size_t(w)*3 || stride>input_capacity/h || bytes<(h-1)*stride+std::size_t(w)*3 || bytes>input_capacity)
      throw std::invalid_argument("PIPELINE_RGB_LAYOUT_OR_BUDGET");
    s.timing={};s.timing.tag=tag;s.timing.started_mono_ns=mono();s.timing.geometry=cuda_letterbox_plan(w,h).geometry;
    s.invalid=false;s.state=State::Prepared;
  }
  void preprocess_rgb(Slot& s,const uint8_t* rgb,std::size_t bytes,unsigned w,unsigned h,std::size_t stride){
    NvtxScope range("preprocess/fused-rgb-letterbox-chw");
    s.pre_start.record(preprocess.value);
    check(launch_rgb_letterbox(rgb,bytes,w,h,stride,static_cast<float*>(s.tensor.value),input_elements,preprocess.value),"PIPELINE_PREPROCESS");
    s.pre_end.record(preprocess.value);
  }
};
GpuYoloPipeline::GpuYoloPipeline(const std::string& model,unsigned slots,GpuInputPath path):impl_(new Impl(model,slots,path)){}
GpuYoloPipeline::~GpuYoloPipeline()=default;
void GpuYoloPipeline::prepare_host(unsigned slot,uint64_t tag,const uint8_t* rgb,std::size_t bytes,unsigned w,unsigned h,std::size_t stride){
  if(!rgb)throw std::invalid_argument("NULL_HOST_RGB");auto& p=*impl_;auto& s=p.get(slot);p.start(s,tag,bytes,w,h,stride);s.external=false;
  try {
    {NvtxScope range("copy/host-staging");std::memcpy(s.host.cpu,rgb,bytes);}
    s.copy_start.record(p.preprocess.value);
    if(!s.mapped){NvtxScope range("copy/pinned-to-device");check(cudaMemcpyAsync(s.rgb->value,s.host.cpu,bytes,cudaMemcpyHostToDevice,p.preprocess.value),"PIPELINE_UPLOAD");}
    s.copy_end.record(p.preprocess.value);
    p.preprocess_rgb(s,static_cast<const uint8_t*>(s.mapped?s.host.gpu:s.rgb->value),bytes,w,h,stride);
  }catch(...){physical_barrier();s.state=State::Free;throw;}
}
void GpuYoloPipeline::prepare_device(unsigned slot,uint64_t tag,const uint8_t* rgb,std::size_t bytes,unsigned w,unsigned h,std::size_t stride,cudaStream_t producer){
  if(!rgb)throw std::invalid_argument("NULL_DEVICE_RGB");auto& p=*impl_;auto& s=p.get(slot);p.start(s,tag,bytes,w,h,stride);s.external=true;
  try {
    s.source_ready.record(producer);check(cudaStreamWaitEvent(p.preprocess.value,s.source_ready.value,0),"PRODUCER_DEPENDENCY");
    s.copy_start.record(p.preprocess.value);s.copy_end.record(p.preprocess.value);
    p.preprocess_rgb(s,rgb,bytes,w,h,stride);
  }catch(...){physical_barrier();s.state=State::Free;throw;}
}
bool GpuYoloPipeline::submit(unsigned slot){
  auto& p=*impl_;auto& s=p.get(slot);if(s.state!=State::Prepared)throw std::logic_error("PIPELINE_NOT_PREPARED");
  if(s.invalid)throw std::logic_error("PIPELINE_JOB_INVALIDATED");
  if(!p.net->context_ready())return false;
  try {
    check(cudaStreamWaitEvent(p.inference.value,s.pre_end.value,0),"PREPROCESS_TO_INFERENCE_DEPENDENCY");
    s.net_start.record(p.inference.value);
    p.net->enqueue_device(static_cast<const float*>(s.tensor.value),static_cast<float*>(s.output.gpu),p.inference.value);
    s.done.record(p.inference.value);s.state=State::InFlight;return true;
  }catch(...){physical_barrier();s.state=State::Free;throw;}
}
bool GpuYoloPipeline::collect(unsigned slot,std::vector<float>& output,GpuStageTiming& timing){
  auto& p=*impl_;auto& s=p.get(slot);if(s.state!=State::InFlight)throw std::logic_error("PIPELINE_NOT_SUBMITTED");
  if(!s.done.ready())return false;
  NvtxScope range("postprocess/collect-completed-output");
  check(cudaEventElapsedTime(&s.timing.preprocess_ms,s.pre_start.value,s.pre_end.value),"PREPROCESS_TIMING");
  check(cudaEventElapsedTime(&s.timing.inference_ms,s.net_start.value,s.done.value),"INFERENCE_TIMING");
  check(cudaEventElapsedTime(&s.timing.critical_path_gpu_ms,s.copy_start.value,s.done.value),"CRITICAL_PATH_TIMING");
  if(!s.mapped && !s.external)check(cudaEventElapsedTime(&s.timing.copy_ms,s.copy_start.value,s.copy_end.value),"COPY_TIMING");
  s.timing.completed_mono_ns=mono();s.timing.valid=!s.invalid;
  if(s.invalid)output.clear();else{output.resize(output_elements);std::memcpy(output.data(),s.output.cpu,output_elements*sizeof(float));}
  timing=s.timing;s.state=State::Free;return true;
}
void GpuYoloPipeline::cancel(unsigned slot){auto& s=impl_->get(slot);if(s.state==State::Free)throw std::logic_error("NO_PIPELINE_JOB_TO_CANCEL");s.invalid=true;}
bool GpuYoloPipeline::discard_prepared(unsigned slot){auto& s=impl_->get(slot);if(s.state!=State::Prepared)throw std::logic_error("NO_PREPARED_JOB");if(!s.pre_end.ready())return false;s.state=State::Free;return true;}
std::string GpuYoloPipeline::capabilities_json()const{
  const auto& p=*impl_;p.require_owner();std::ostringstream out;
  out << "{\"gpu\":\"" << p.properties.name << "\",\"slots\":" << p.slots.size() << ",\"input_path\":\"" << (p.path==GpuInputPath::Mapped?"mapped":"pinned-device")
      << "\",\"integrated\":" << p.properties.integrated << ",\"can_map_host\":" << p.properties.canMapHostMemory << ",\"host_register_supported\":" << p.host_register
      << ",\"managed_memory\":" << p.properties.managedMemory << ",\"concurrent_managed_access\":" << p.properties.concurrentManagedAccess << ",\"async_engines\":" << p.properties.asyncEngineCount
      << ",\"cuda_runtime\":" << p.runtime_version << ",\"cuda_driver\":" << p.driver_version << ",\"source_capacity_per_slot\":" << input_capacity << ",\"default_stream_used_for_inference\":false}";
  return out.str();
}
} // namespace edgevision
