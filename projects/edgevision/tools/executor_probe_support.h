#pragma once
// Shared real-device acceptance harness. No product route loads these hooks.
#include "edgevision/upstream_executor.h"
#include <cuda_runtime.h>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <thread>
extern "C" cudaError_t edgevision_acceptance_delay(unsigned milliseconds);
using namespace edgevision;
namespace edgevision::acceptance {
inline uint64_t now() { return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
inline void require(bool ok,const std::string& error) { if(!ok)throw std::runtime_error(error); }
inline void cuda_check(cudaError_t e) { if(e!=cudaSuccess)throw std::runtime_error(std::string("CUDA_TEST_OBSERVER: ")+cudaGetErrorString(e)); }
inline std::string quote(const std::string& value) {
  std::string out="\"";for(unsigned char c:value){if(c=='"'||c=='\\')out+='\\';out+=c<32?'?':char(c);}return out+'"';
}
inline const char* name(TaskState state) {
  switch(state){case TaskState::Queued:return "QUEUED";case TaskState::Running:return "RUNNING";
    case TaskState::Done:return "DONE";case TaskState::Failed:return "FAILED";case TaskState::Expired:return "EXPIRED";case TaskState::Canceled:return "CANCELED";}return "UNKNOWN";
}
inline const char* name(CaptureState state) {
  switch(state){case CaptureState::Idle:return "IDLE";case CaptureState::Starting:return "STARTING";
    case CaptureState::Running:return "RUNNING";case CaptureState::Stopping:return "STOPPING";
    case CaptureState::Recovering:return "RECOVERING";case CaptureState::Error:return "ERROR";}return "UNKNOWN";
}
inline std::size_t video_maps() {
  std::ifstream file("/proc/self/maps");std::string line;std::size_t n=0;
  while(std::getline(file,line))if(line.find("/dev/video")!=std::string::npos)++n;return n;
}
struct Gate {
  cudaEvent_t event=nullptr;
  std::atomic<bool> enabled{false};
  std::atomic<unsigned> records{0};
  Gate(){cuda_check(cudaEventCreateWithFlags(&event,cudaEventDisableTiming));}
  ~Gate(){if(event)cudaEventDestroy(event);}
  void enqueue(){if(enabled){cuda_check(edgevision_acceptance_delay(200));cuda_check(cudaEventRecord(event,0));++records;}}
  bool pending()const {auto e=cudaEventQuery(event);if(e==cudaErrorNotReady)return true;cuda_check(e);return false;}
};
struct Harness {
  Gate gate;V4L2Backend capture;HealthMonitor health;UpstreamExecutor executor;ControlRouter router;
  CaptureConfig config;std::ofstream events;uint64_t sample_at=0,max_poll_ns=0;unsigned cases=0;
  Harness(const std::string& device,const std::string& out,std::size_t retained=256):executor(capture,&health,[&]{gate.enqueue();}),router(capture,&executor,retained),events(out+"/events.jsonl") {
    config.device=device;events.exceptions(std::ios::badbit|std::ios::failbit);
  }
  ~Harness(){
    try{router.shutdown(now());const auto end=now()+15000000000ULL;
      while(!router.status().shutdown_complete){step();if(now()>end)throw std::runtime_error("CLEANUP_TIMEOUT");std::this_thread::sleep_for(std::chrono::milliseconds(1));}}
    catch(...){std::fputs("EXECUTOR_PROBE_CLEANUP_UNKNOWN_PROCESS_ISOLATION\n",stderr);std::fflush(nullptr);std::_Exit(74);}
  }
  void step(){const auto begin=now();router.poll(begin);const auto elapsed=now()-begin;if(elapsed>max_poll_ns)max_poll_ns=elapsed;
    if(begin>=sample_at){health.observe_capture(capture.health_sample(begin));sample_at=begin+10000000;}}
  template<class Predicate> void wait(Predicate predicate,uint64_t budget=10000000000ULL){
    const auto end=now()+budget;
    do{step();if(predicate())return;std::this_thread::sleep_for(std::chrono::milliseconds(1));}while(now()<end);
    throw std::runtime_error("WAIT_BUDGET_EXCEEDED");
  }
  TaskAdmission detect(const std::string& id,uint64_t budget=5000000000ULL){
    const auto at=now();auto a=router.submit({id,router.status().generation,at+budget,TaskCommand::DetectSnapshot,{}},at);
    require(a.accepted,"DETECT_ADMISSION_REJECTED:"+id);return a;
  }
  TaskRecord done(uint64_t id){wait([&]{return bool(router.collect(id));});return *router.collect(id);}
  void expect(uint64_t id,TaskState wanted){auto record=done(id);require(record.state==wanted,"UNEXPECTED_TASK_STATE:"+record.request.request_id);
    require(!record.execution_pending,"PHYSICAL_PENDING_AFTER_COLLECT");if(wanted!=TaskState::Done)require(record.result.empty(),"INVALID_RESULT_PUBLISHED");}
  void start(const std::string& id){const auto at=now();auto a=router.submit({id,router.status().generation,at+10000000000ULL,TaskCommand::StartCapture,config},at);
    require(a.accepted,"START_REJECTED");expect(a.task_id,TaskState::Done);require(router.status().capture==CaptureState::Running,"NO_FIRST_FRAME");}
  void gpu_pending(uint64_t id){
    wait([&]{const auto s=executor.query();return s.task_id==id&&s.phase==ExecutionPhase::InFlight;});
    const auto s=executor.query();require(gate.pending(),"REAL_GPU_PENDING_WINDOW_NOT_OBSERVED");
    require(s.physical_busy&&s.held_frame_id&&s.held_slot_state==SlotState::InFlight,"LEASE_NOT_INFLIGHT");
  }
  void log(const std::string& event,std::optional<uint64_t> id={}){
    const auto r=router.status();const auto e=executor.query();const auto c=capture.snapshot();
    const bool device_pending=gate.pending();
    if(event=="expired_while_gpu_pending" || event=="canceled_while_gpu_pending" ||
       event=="stop_waits_for_inflight_lease" || event=="shutdown_keeps_gpu_resources")
      require(device_pending,"GPU_COMPLETED_BEFORE_EVIDENCE_SNAPSHOT");
    events<<"{\"mono_ns\":"<<now()<<",\"event\":"<<quote(event)<<",\"capture\":"<<quote(name(r.capture))
      <<",\"generation\":"<<r.generation<<",\"start_retries\":"<<r.start_retries<<",\"model_state\":"<<int(r.model)<<",\"capture_resources_live\":"<<(r.backend_resources_live?"true":"false")
      <<",\"queue_depth\":"<<c.queue_depth<<",\"pool_busy\":"<<c.pool_busy<<",\"gpu_phase\":"<<int(e.phase)
      <<",\"gpu_submissions\":"<<e.gpu_submissions<<",\"gpu_completions\":"<<e.gpu_completions<<",\"gpu_event_pending\":"<<(device_pending?"true":"false")
      <<",\"gpu_submitted_mono_ns\":"<<e.gpu_submitted_mono_ns
      <<",\"gpu_physical_busy\":"<<(e.physical_busy?"true":"false")<<",\"shutdown_complete\":"<<(r.shutdown_complete?"true":"false");
    if(id){const auto t=router.query(*id);require(bool(t),"MISSING_TASK");events<<",\"task_id\":"<<*id<<",\"task_state\":"<<quote(name(t->state))
      <<",\"execution_pending\":"<<(t->execution_pending?"true":"false")<<",\"deadline_mono_ns\":"<<t->request.deadline_mono_ns
      <<",\"error\":"<<quote(t->error_code)<<",\"result\":"<<quote(t->result);}
    events<<"}\n";events.flush();std::cout<<event<<std::endl;
  }
  void check_pending(uint64_t id,TaskState state,const std::string& event){const auto r=router.query(id);
    require(r&&r->state==state&&r->execution_pending&&!router.collect(id),"LOGICAL_TERMINAL_RELEASED_PHYSICAL_SLOT");
    require(gate.pending(),"GPU_ALREADY_COMPLETED_BEFORE_TERMINAL_OBSERVATION");log(event,id);}
};
}
