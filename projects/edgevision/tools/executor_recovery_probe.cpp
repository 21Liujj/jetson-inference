// Operator-assisted real USB recovery with the real task/GPU executor alive.
// Explicit 200ms test GPU work and a 4096-record test history budget are logged.
#include "executor_probe_support.h"
#include <csignal>
using namespace edgevision;
using namespace edgevision::acceptance;
namespace {volatile std::sig_atomic_t interrupted=0;void signal_handler(int){interrupted=1;}}
int main(int argc,char** argv){
  std::string out;
  try{
    std::string device,model,labels;
    for(int i=1;i<argc;++i){require(i+1<argc,"OPTION_NEEDS_VALUE");const std::string key=argv[i],value=argv[++i];
      if(key=="--device")device=value;else if(key=="--model")model=value;else if(key=="--labels")labels=value;else if(key=="--out")out=value;else throw std::invalid_argument("unknown option");}
    require(!device.empty()&&!model.empty()&&!labels.empty()&&!out.empty(),"INVALID_ARGUMENTS");
    require(std::filesystem::create_directories(out),"OUTPUT_ALREADY_EXISTS");
    Harness h(device,out,4096);h.config.runtime_retry_limit=5;h.config.runtime_recovery_budget_ms=30000;
    require(h.router.initialize({model,labels,250}).accepted,"INITIALIZE_REJECTED");h.start("recovery-start");
    h.wait([&]{return h.router.status().model==ModelState::Ready;},45000000000ULL);
    auto warm=h.detect("recovery-warmup");h.expect(warm.task_id,TaskState::Done);h.gate.enabled=true;
    std::signal(SIGINT,signal_handler);std::signal(SIGTERM,signal_handler);
    std::ofstream tasks(out+"/tasks.jsonl");tasks.exceptions(std::ios::badbit|std::ios::failbit);
    const auto initial_generation=h.router.status().generation,begin=now();
    uint64_t next_submit=0,number=0;std::optional<uint64_t> active;
    bool ready=false,fault_seen=false,recovered=false,fault_gpu_pending=false;
    uint64_t fault_at=0;unsigned valid_after_recovery=0,done=0,invalid=0;
    CaptureState previous=CaptureState::Running;
    while(!interrupted&&now()-begin<600000000000ULL){
      h.step();const auto state=h.router.status();
      if(state.capture!=previous){h.log("capture_state",active);previous=state.capture;}
      if(active&&h.executor.query().phase==ExecutionPhase::InFlight&&!ready){ready=true;h.log("ready_for_disconnect",active);}
      if(ready&&!fault_seen&&state.capture!=CaptureState::Running){
        fault_seen=true;fault_at=state.fault_detected_mono_ns;fault_gpu_pending=h.gate.pending();h.log("disconnect_observed",active);
      }
      if(state.capture==CaptureState::Error)throw std::runtime_error("RECOVERY_FAILED:"+state.last_error);
      if(active){
        auto record=h.router.collect(*active);
        if(record){
          tasks<<"{\"mono_ns\":"<<now()<<",\"task_id\":"<<record->id<<",\"request_generation\":"<<record->request.generation
               <<",\"state\":"<<quote(name(record->state))<<",\"execution_pending\":false,\"error\":"<<quote(record->error_code)
               <<",\"result\":"<<quote(record->result)<<"}\n";tasks.flush();
          if(record->state==TaskState::Done){++done;if(fault_seen&&record->request.generation>initial_generation)++valid_after_recovery;}
          else{++invalid;require(record->result.empty(),"INVALID_TASK_PUBLISHED_RESULT");}
          active.reset();
        }
      }
      if(fault_seen&&!recovered&&state.capture==CaptureState::Running&&state.generation>initial_generation){
        recovered=true;h.log("new_generation_running");
      }
      if(recovered&&valid_after_recovery>=2)break;
      if(fault_seen&&now()-fault_at>45000000000ULL)throw std::runtime_error("NO_VALID_POST_RECOVERY_TASK");
      if(!active&&state.capture==CaptureState::Running&&state.model==ModelState::Ready&&now()>=next_submit){
        active=h.detect("recovery-detect-"+std::to_string(number++)).task_id;next_submit=now()+250000000;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    require(!interrupted,"INTERRUPTED");require(fault_seen&&recovered&&valid_after_recovery>=2,"RECOVERY_NOT_OBSERVED");
    const auto recovered_state=h.router.status();h.router.shutdown(now());h.wait([&]{return h.router.status().shutdown_complete;});
    require(h.capture.snapshot().pool_busy==0&&video_maps()==0,"FINAL_RESOURCES_LIVE");h.log("final");
    auto e=h.executor.query();std::ofstream summary(out+"/summary.json");
    summary<<"{\"scope\":\"real USB recovery with common task/GPU executor; explicit CUDA injection, no benchmark\",\"fault_seen\":true,\"recovered\":true"
      <<",\"fault_observed_with_gpu_event_pending\":"<<(fault_gpu_pending?"true":"false")<<",\"fault_detected_ns\":"<<fault_at
      <<",\"recovered_ns\":"<<recovered_state.recovered_mono_ns<<",\"generation\":"<<recovered_state.generation
      <<",\"runtime_retries\":"<<recovered_state.runtime_retries<<",\"runtime_retry_limit\":5,\"recovery_budget_ms\":30000,\"retained_task_budget\":4096"
      <<",\"valid_tasks_after_recovery\":"<<valid_after_recovery<<",\"done_tasks\":"<<done<<",\"invalid_tasks\":"<<invalid
      <<",\"gpu_submissions\":"<<e.gpu_submissions<<",\"gpu_completions\":"<<e.gpu_completions<<",\"explicit_cuda_delay_injection_ms\":200"
      <<",\"pool_busy\":0,\"video_maps\":0,\"shutdown_complete\":true,\"failure\":\"\"}\n";
    std::cout<<"PASS joint task/GPU/USB recovery"<<std::endl;return 0;
  }catch(const std::exception& e){std::cerr<<"EXECUTOR_RECOVERY_PROBE_FAILED: "<<e.what()<<std::endl;
    if(!out.empty()&&std::filesystem::exists(out)){std::ofstream failure(out+"/failure.json");failure<<"{\"failure\":"<<quote(e.what())<<"}\n";}return 1;}
}
