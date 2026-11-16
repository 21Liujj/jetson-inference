#include "edgevision/cuda_letterbox.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <cstdlib>

namespace fs=std::filesystem;
using namespace edgevision;
void checked(cudaError_t e,const char* name) {if(e!=cudaSuccess) throw std::runtime_error(std::string(name)+":"+cudaGetErrorString(e));}
struct Buffers {
  static constexpr std::size_t capacity=4*1024*1024;
  cudaStream_t stream=nullptr;void* host=nullptr;void* device=nullptr;void* input=nullptr;float* output=nullptr;
  bool mapped;
  explicit Buffers(bool use_mapped):mapped(use_mapped) {
    try {
      checked(cudaStreamCreateWithFlags(&stream,cudaStreamNonBlocking),"stream");
      checked(cudaHostAlloc(&host,capacity,mapped?cudaHostAllocMapped:cudaHostAllocDefault),"pinned_input");
      if(mapped) checked(cudaHostGetDevicePointer(&input,host,0),"mapped_alias");
      else {checked(cudaMalloc(&device,capacity),"device_input");input=device;}
      checked(cudaMalloc(reinterpret_cast<void**>(&output),3*640*640*sizeof(float)),"tensor");
    } catch(...) {release();throw;}
  }
  void release() noexcept {
    if(stream && cudaStreamSynchronize(stream)!=cudaSuccess) std::_Exit(70);
    if(output)cudaFree(output);if(device)cudaFree(device);if(host)cudaFreeHost(host);if(stream)cudaStreamDestroy(stream);
    output=nullptr;device=host=input=nullptr;stream=nullptr;
  }
  ~Buffers(){release();}
};
int main(int argc,char** argv) {
  try {
    if(argc!=3) throw std::invalid_argument("usage: edgevision_cuda_preprocess_probe CORPUS NEW_OUT");
    const fs::path corpus=argv[1],out=argv[2]; if(!fs::create_directory(out)) throw std::runtime_error("OUTPUT_MUST_BE_NEW");
    cudaDeviceProp prop{};checked(cudaGetDeviceProperties(&prop,0),"properties");
    std::ofstream info(out/"capabilities.json");
    info << "{\"gpu\":\"" << prop.name << "\",\"can_map_host\":" << prop.canMapHostMemory
      << ",\"managed_memory\":" << prop.managedMemory << ",\"concurrent_managed_access\":" << prop.concurrentManagedAccess
      << ",\"async_engines\":" << prop.asyncEngineCount << ",\"integrated\":" << prop.integrated << "}\n";
    if(!prop.canMapHostMemory)throw std::runtime_error("MAPPED_PATH_UNSUPPORTED");
    std::ofstream log(out/"errors.jsonl");log.precision(10);
    unsigned cases=0;bool all=true;double maximum_all=0,mean_max=0;
    for(bool mapped:{false,true}) {
      Buffers buffers(mapped); std::ifstream list(corpus/"inputs.tsv");std::string line;
      if(!list)throw std::runtime_error("CORPUS_OPEN_FAILED");
      while(std::getline(list,line)) {
        std::istringstream row(line);int id,w,h,left,top,fixed;float scale;
        if(!(row>>id>>w>>h>>left>>top>>scale>>fixed))throw std::runtime_error("INVALID_CORPUS");
        if(!fixed)continue;
        if(id<0 || w<1 || h<1 || w>8192 || h>8192)throw std::runtime_error("INVALID_GEOMETRY");
        const std::size_t stride=std::size_t(w)*3+17,bytes=stride*h;
        if(bytes>buffers.capacity)throw std::runtime_error("INPUT_BUDGET_EXCEEDED");
        std::vector<uint8_t> rgb(std::size_t(w)*h*3),reference(3*640*640);
        std::ifstream image(corpus/(std::to_string(id)+".rgb"),std::ios::binary),gold(corpus/(std::to_string(id)+".u8"),std::ios::binary);
        if(!image.read(reinterpret_cast<char*>(rgb.data()),rgb.size()) || !gold.read(reinterpret_cast<char*>(reference.data()),reference.size()))
          throw std::runtime_error("REFERENCE_READ_FAILED");
        std::fill_n(static_cast<uint8_t*>(buffers.host),bytes,uint8_t(213));
        for(int y=0;y<h;++y)std::copy_n(rgb.data()+std::size_t(y)*w*3,w*3,static_cast<uint8_t*>(buffers.host)+y*stride);
        if(!mapped)checked(cudaMemcpyAsync(buffers.device,buffers.host,bytes,cudaMemcpyHostToDevice,buffers.stream),"upload");
        const auto plan=cuda_letterbox_plan(w,h);
        if(plan.geometry.left!=left || plan.geometry.top!=top || std::abs(plan.geometry.scale-scale)>1e-6)throw std::runtime_error("GEOMETRY_DRIFT");
        checked(launch_rgb_letterbox(static_cast<const uint8_t*>(buffers.input),bytes,w,h,stride,buffers.output,reference.size(),buffers.stream),"letterbox");
        std::vector<float> actual(reference.size());
        checked(cudaMemcpyAsync(actual.data(),buffers.output,actual.size()*sizeof(float),cudaMemcpyDeviceToHost,buffers.stream),"download");
        checked(cudaStreamSynchronize(buffers.stream),"actual_completion");
        double maximum=0,sum=0;uint64_t changed=0;
        for(std::size_t i=0;i<actual.size();++i) {
          if(!std::isfinite(actual[i]))throw std::runtime_error("NONFINITE_PREPROCESS");
          const double d=std::abs(double(actual[i])-double(float(reference[i])/255.f));
          maximum=std::max(maximum,d);sum+=d;changed+=d>1e-6;
        }
        const double mean=sum/actual.size();const bool pass=maximum<=.00392256862745098 && mean<=.001;
        maximum_all=std::max(maximum_all,maximum);mean_max=std::max(mean_max,mean);all &= pass;
        const std::string name=std::string(mapped?"mapped-":"pinned-")+std::to_string(id);
        log << "{\"case\":\"" << name << "\",\"max_abs\":" << maximum << ",\"mean_abs\":" << mean << ",\"changed\":" << changed << ",\"pass\":" << (pass?"true":"false") << "}\n";log.flush();
        std::ofstream raw(out/(name+".f32"),std::ios::binary);
        if(!raw.write(reinterpret_cast<const char*>(actual.data()),actual.size()*sizeof(float)))throw std::runtime_error("RAW_OUTPUT_WRITE_FAILED");
        ++cases;
      }
      if(launch_rgb_letterbox(static_cast<const uint8_t*>(buffers.input),1,640,360,1920,buffers.output,3*640*640,buffers.stream)!=cudaErrorInvalidValue)
        throw std::runtime_error("TRUNCATED_INPUT_NOT_REJECTED");
    }
    all &= cases>=48;
    std::ofstream summary(out/"summary.json");summary.precision(10);
    summary << "{\"cases\":" << cases << ",\"max_abs\":" << maximum_all << ",\"max_mean_abs\":" << mean_max << ",\"pass\":" << (all?"true":"false") << "}\n";
    std::cout << "CUDA_PREPROCESS_" << (all?"PASS":"FAIL") << " cases=" << cases << " maximum=" << maximum_all << " mean_max=" << mean_max << '\n';
    return all?0:1;
  }catch(const std::exception& e){std::cerr << "CUDA_PREPROCESS_FAILED: " << e.what() << '\n';return 1;}
}
