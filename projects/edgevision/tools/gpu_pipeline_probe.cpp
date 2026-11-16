#include "edgevision/gpu_yolo_pipeline.h"
#include "edgevision/upstream_yolo.h"
#include "edgevision/yolo_preprocess.h"
#include "edgevision/nvtx_scope.h"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <thread>

using namespace edgevision;
namespace fs=std::filesystem;
uint64_t mono(){return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();}
struct Image {uint64_t id=0;unsigned w=0,h=0;bool fixed=false;std::vector<uint8_t> rgb;};
void boxes(std::ostream& o,const std::vector<YoloBox>& b){o<<'[';for(std::size_t i=0;i<b.size();++i){if(i)o<<',';const auto& v=b[i];o<<'['<<v.anchor<<','<<v.class_id<<','<<v.score<<','<<v.left<<','<<v.top<<','<<v.right<<','<<v.bottom<<']';}o<<']';}
int main(int argc,char** argv){
  try{
    if(argc<5 || argc>9)throw std::invalid_argument("usage: edgevision_gpu_pipeline_probe MODEL CORPUS NEW_OUT cpu|p1|p2 [pinned|mapped] [iterations,0=validate-all] [warmup] [measured-seconds,0=count-only]");
    std::string mode=argv[4],memory=argc>5?argv[5]:"pinned";
    const unsigned iterations=argc>6?std::stoul(argv[6]):0,warmup=argc>7?std::stoul(argv[7]):30;
    const unsigned seconds=argc>8?std::stoul(argv[8]):0;
    if((mode!="cpu"&&mode!="p1"&&mode!="p2") || (memory!="pinned"&&memory!="mapped") || iterations>100000 || warmup>1000 || seconds>1800 || (seconds&&!iterations))throw std::invalid_argument("INVALID_RUN_BUDGET");
    const fs::path corpus=argv[2],out=argv[3];if(!fs::create_directory(out))throw std::runtime_error("OUTPUT_MUST_BE_NEW");
    std::vector<Image> images;std::ifstream list(corpus/"inputs.tsv");std::string line;
    while(std::getline(list,line)){
      std::istringstream row(line);Image image;int left,top,fixed;float scale;
      if(!(row>>image.id>>image.w>>image.h>>left>>top>>scale>>fixed) || image.w<1 || image.h<1 || image.w>8192 || image.h>8192)throw std::runtime_error("INVALID_CORPUS");
      if(iterations && !fixed)continue;
      image.fixed=fixed;image.rgb.resize(std::size_t(image.w)*image.h*3);
      std::ifstream rgb(corpus/(std::to_string(image.id)+".rgb"),std::ios::binary);
      if(!rgb.read(reinterpret_cast<char*>(image.rgb.data()),image.rgb.size()))throw std::runtime_error("RGB_INPUT_MISSING");
      images.push_back(std::move(image));
    }
    if(images.size()<20)throw std::runtime_error("INSUFFICIENT_INPUT_CASES");
    std::unique_ptr<UpstreamYolo> cpu;
    std::unique_ptr<GpuYoloPipeline> gpu;
    if(mode=="cpu"){cpu=std::make_unique<UpstreamYolo>();cpu->initialize(argv[1]);}
    else{gpu=std::make_unique<GpuYoloPipeline>(argv[1],mode=="p2"?2:1,memory=="mapped"?GpuInputPath::Mapped:GpuInputPath::PinnedDevice);std::ofstream cap(out/"capabilities.json");cap<<gpu->capabilities_json()<<'\n';}
    std::ofstream detection(out/"detections.jsonl"),timings(out/"timings.csv");detection<<std::setprecision(9);
    timings<<"ordinal,image_id,started_mono_ns,finished_mono_ns,latency_ns,copy_ms,preprocess_ms,inference_ms,critical_path_gpu_ms,boxes\n";
    std::vector<float> output;output.reserve(84*8400);std::vector<uint8_t> tensor_bytes;std::vector<float> normalized;
    std::vector<uint64_t> latencies;std::vector<double> pre_times,net_times;uint64_t measured_ns=0;unsigned completed=0;
    auto result=[&](unsigned ordinal,const Image& image,const std::vector<float>& raw,LetterboxGeometry geometry,uint64_t begin,GpuStageTiming t,bool measure){
      NvtxScope post("postprocess/common-yolo-nms");
      auto display=decode_yolov8(raw.data(),raw.size(),geometry);
      // Accuracy validation includes multi-label evaluation; formal speed runs
      // use the SAME display NMS for every P0/P1/P2 variant.
      if(!iterations && measure){auto eval=decode_yolov8(raw.data(),raw.size(),geometry,.001f,true);
        detection<<"{\"image_id\":"<<image.id<<",\"display\":";boxes(detection,display);detection<<",\"evaluation\":";boxes(detection,eval);detection<<"}\n";
        if(image.fixed){std::ofstream saved(out/(std::to_string(image.id)+".f32"),std::ios::binary);if(!saved.write(reinterpret_cast<const char*>(raw.data()),raw.size()*sizeof(float)))throw std::runtime_error("RAW_WRITE_FAILED");}}
      const auto end=mono();
      if(measure){latencies.push_back(end-begin);pre_times.push_back(t.preprocess_ms);net_times.push_back(t.inference_ms);++completed;
        timings<<ordinal<<','<<image.id<<','<<begin<<','<<end<<','<<end-begin<<','<<t.copy_ms<<','<<t.preprocess_ms<<','<<t.inference_ms<<','<<t.critical_path_gpu_ms<<','<<display.size()<<'\n';}
    };
    auto run=[&](unsigned count,bool measure){
      const auto started=mono();
      const auto elapsed_budget=[&]{return measure&&seconds&&mono()-started>=uint64_t(seconds)*1000000000ULL;};
      if(cpu){
        for(unsigned n=0;n<count&&!elapsed_budget();++n){const auto& image=images[n%images.size()];const auto begin=mono();LetterboxGeometry geometry;
          {NvtxScope pre("preprocess/P0-cpu-letterbox");geometry=yolo_letterbox_rgb(image.rgb.data(),image.rgb.size(),image.w,image.h,image.w*3,tensor_bytes);
           normalized.resize(tensor_bytes.size());for(std::size_t j=0;j<normalized.size();++j)normalized[j]=float(tensor_bytes[j])/255.f;}
          auto raw=cpu->infer(normalized.data(),normalized.size());result(n,image,raw,geometry,begin,{},measure);}
      }else{
        struct Pending{bool used=false,submitted=false;unsigned ordinal=0;};Pending slots[2];const unsigned capacity=mode=="p2"?2:1;
        unsigned next=0,finished=0;
        auto fill=[&](unsigned s){if(next>=count||elapsed_budget())return;const auto& image=images[next%images.size()];
          gpu->prepare_host(s,next,image.rgb.data(),image.rgb.size(),image.w,image.h,image.w*3);slots[s]={true,false,next++};};
        for(unsigned s=0;s<capacity;++s)fill(s);
        while(finished<next || (next<count&&!elapsed_budget())){
          bool progressed=false;
          for(unsigned s=0;s<capacity;++s)if(slots[s].used && !slots[s].submitted){if(gpu->submit(s)){slots[s].submitted=true;progressed=true;}}
          for(unsigned s=0;s<capacity;++s)if(slots[s].used && slots[s].submitted){GpuStageTiming t;
            if(!gpu->collect(s,output,t))continue;
            if(!t.valid || t.tag!=slots[s].ordinal)throw std::runtime_error("PIPELINE_RESULT_IDENTITY");
            const unsigned ordinal=slots[s].ordinal;slots[s].used=false;
            // Start the next prepared network job before CPU NMS, then prepare
            // the freed slot while that network may execute on the other stream.
            for(unsigned j=0;j<capacity;++j)if(slots[j].used&&!slots[j].submitted)
              if(gpu->submit(j))slots[j].submitted=true;
            fill(s);result(ordinal,images[ordinal%images.size()],output,t.geometry,t.started_mono_ns,t,measure);
            ++finished;progressed=true;
          }
          if(!progressed)std::this_thread::sleep_for(std::chrono::microseconds(25));
        }
      }
      if(measure){measured_ns=mono()-started;
        if(seconds&&measured_ns<uint64_t(seconds)*1000000000ULL)throw std::runtime_error("DURATION_COUNT_SAFETY_BUDGET_REACHED");}
    };
    run(warmup,false);run(seconds?2000000:(iterations?iterations:unsigned(images.size())),true);
    std::sort(latencies.begin(),latencies.end());
    auto percentile=[&](double p){return latencies[std::min(latencies.size()-1,std::size_t(std::ceil(latencies.size()*p)-1))];};
    const auto mean=[](const std::vector<double>& x){double sum=0;for(auto v:x)sum+=v;return x.empty()?0:sum/x.size();};
    std::ofstream summary(out/"summary.json");summary<<std::setprecision(12);
    summary<<"{\"mode\":\""<<mode<<"\",\"memory\":\""<<memory<<"\",\"warmup_excluded\":"<<warmup<<",\"completed\":"<<completed
      <<",\"requested_measured_seconds\":"<<seconds<<",\"bounded_timing_csv_included_in_throughput\":true"
      <<",\"measured_ns\":"<<measured_ns<<",\"throughput_per_s\":"<<double(completed)*1e9/measured_ns
      <<",\"latency_p50_ns\":"<<percentile(.5)<<",\"latency_p95_ns\":"<<percentile(.95)<<",\"latency_p99_ns\":"<<percentile(.99)
      <<",\"preprocess_event_mean_ms\":"<<mean(pre_times)<<",\"inference_event_mean_ms\":"<<mean(net_times)
      <<",\"latency_scope\":\"host prepare through actual GPU completion and common display NMS; file writes excluded in performance mode\",\"camera_throughput_claim\":false,\"normal_exit\":true}\n";
    std::cout<<"PIPELINE_PROBE_PASS mode="<<mode<<" completed="<<completed<<'\n';return 0;
  }catch(const std::exception& e){std::cerr<<"PIPELINE_PROBE_FAILED: "<<e.what()<<'\n';return 1;}
}
