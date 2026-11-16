#include "edgevision/upstream_executor.h"
#include "edgevision/upstream_detector.h"
#ifdef EDGEVISION_HAS_AGENT_BACKEND
#include "edgevision/agent_tools.h"
#include <cuda_runtime_api.h>
#ifdef __GLIBC__
#include <malloc.h>
#endif
#endif
#include <algorithm>
#include <chrono>
#include <sstream>
#include <stdexcept>

namespace edgevision {
namespace {
uint64_t now() { return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
std::string quote(const std::string& value) {
  std::string out = "\"";
  for (unsigned char c : value) { if (c == '"' || c == '\\') out += '\\'; out += c < 32 ? '?' : char(c); }
  return out + '"';
}
std::string serialize(uint64_t task, const DetectionResult& result,const std::string& snapshot_id="") {
  // Wire result is bounded independently of upstream's maximum 3000 boxes.
  const auto included = std::min<std::size_t>(64, result.boxes.size());
  std::ostringstream out;
  out << "{\"schema_version\":1,\"task_id\":" << task << ",\"generation\":" << result.frame.session_generation
      << ",\"frame_id\":" << result.frame.frame_id << ",\"sequence\":" << result.frame.driver_sequence
      << ",\"width\":" << result.frame.width << ",\"height\":" << result.frame.height
      << ",\"host_dq_ns\":" << result.frame.host_dequeue_mono_ns << ",\"completed_mono_ns\":" << result.completed_mono_ns
      << ",\"driver_timestamp_valid\":" << (result.frame.driver_timestamp_ns ? "true":"false")
      << ",\"driver_timestamp_ns\":" << result.frame.driver_timestamp_ns.value_or(0)
      << ",\"driver_clock_monotonic\":" << (result.frame.driver_timestamp_clock==FrameMeta::TimestampClock::Monotonic?"true":"false")
      << ",\"driver_timestamp_source\":" << unsigned(result.frame.driver_timestamp_source)
      << ",\"driver_sequence\":" << result.frame.driver_sequence << ",\"driver_flags\":" << result.frame.driver_flags
      << ",\"stride\":" << result.frame.stride << ",\"bytesused\":" << result.frame.bytesused
      << ",\"fourcc\":" << result.frame.pixfmt_fourcc << ",\"publish_mono_ns\":" << result.frame.publish_mono_ns
      << ",\"snapshot_id\":" << quote(snapshot_id) << ",\"input_source\":" << quote(snapshot_id.empty()?"live_camera":"stored_snapshot")
      << ",\"detections_total\":" << result.boxes.size() << ",\"detections_truncated\":" << (included < result.boxes.size() ? "true" : "false") << ",\"detections\":[";
  for (std::size_t i=0; i<included; ++i) {
    const auto& b = result.boxes[i]; if (b.label.size()>128) throw std::runtime_error("RESULT_LABEL_BUDGET");
    if (i) out << ',';
    out << "{\"class_id\":" << b.class_id << ",\"label\":" << quote(b.label) << ",\"confidence\":" << b.confidence
        << ",\"left\":" << b.left << ",\"top\":" << b.top << ",\"right\":" << b.right << ",\"bottom\":" << b.bottom << '}';
  }
  out << "]}";
  auto text = out.str(); if (text.size()>16384) throw std::runtime_error("RESULT_BUDGET_EXCEEDED"); return text;
}
}
UpstreamExecutor::UpstreamExecutor(V4L2Backend& capture, HealthMonitor* health, std::function<void()> hook,
                                   std::shared_ptr<PreviewCache> preview)
    : capture_(capture), health_(health), acceptance_gpu_work_(std::move(hook)), inference_preview_(std::move(preview)),
      worker_(&UpstreamExecutor::worker_loop, this) {}
UpstreamExecutor::~UpstreamExecutor() { shutdown(); if (worker_.joinable()) worker_.join(); }
bool UpstreamExecutor::supports_command(TaskCommand command)const noexcept {
  return command==TaskCommand::DetectSnapshot || (agent_enabled_ && (command==TaskCommand::CaptureSnapshot || command==TaskCommand::ResumeDetector)) ||
    (vlm_enabled_ && (command==TaskCommand::PlanTool || command==TaskCommand::DescribeSnapshot));
}
ExecutionReply UpstreamExecutor::initialize(InferenceConfig config) {
  std::lock_guard<std::mutex> lock(mu_);
  if (closing_ || config_) return {false, "MODEL_ALREADY_INITIALIZED_OR_CLOSING"};
  if (config.onnx.empty() || config.labels.empty() || config.onnx.size()>512 || config.labels.size()>512 ||
      config.onnx.find('\0')!=std::string::npos || config.labels.find('\0')!=std::string::npos ||
      !config.max_frame_age_ms || config.max_frame_age_ms>5000 ||
      (config.kind!=DetectorKind::SsdFruit && config.kind!=DetectorKind::YoloV8Coco) ||
      (config.preprocess!=PreprocessMode::CpuBaseline && config.preprocess!=PreprocessMode::CudaFused) ||
      (config.preprocess==PreprocessMode::CudaFused && config.kind!=DetectorKind::YoloV8Coco)) return {false, "INVALID_MODEL_CONFIG"};
  const auto& agent=config.agent;
  for(const auto* value:{&agent.snapshot_root,&agent.vlm_binary,&agent.llm_engine_dir,&agent.visual_engine_dir,&agent.plugin_path})
    if(value->size()>512||value->find('\0')!=std::string::npos)return {false,"INVALID_AGENT_PATH"};
  if(!agent.max_snapshots||agent.max_snapshots>64||agent.vlm_idle_restore_ms<2000||agent.vlm_idle_restore_ms>60000||
     !agent.vlm_max_requests||agent.vlm_max_requests>64)return {false,"INVALID_AGENT_BUDGET"};
  if(!agent.vlm_binary.empty()&&(agent.snapshot_root.empty()||agent.llm_engine_dir.empty()||agent.visual_engine_dir.empty()||agent.plugin_path.empty()))
    return {false,"INCOMPLETE_VLM_CONFIG"};
#ifndef EDGEVISION_HAS_AGENT_BACKEND
  if(!agent.snapshot_root.empty()||!agent.vlm_binary.empty())return {false,"AGENT_BACKEND_NOT_BUILT"};
#endif
  agent_enabled_=!agent.snapshot_root.empty();vlm_enabled_=!agent.vlm_binary.empty();
  config_ = std::move(config); state_.model = ModelState::Loading; wake_.notify_one(); return {true, ""};
}
ExecutionReply UpstreamExecutor::submit(ExecutionJob job) {
  std::lock_guard<std::mutex> lock(mu_);
  if (closing_ || !state_.accepting) return {false, "MODEL_NOT_READY"};
  if(!supports_command(job.command))return {false,"EXECUTOR_UNSUPPORTED_COMMAND"};
  if(job.command==TaskCommand::DetectSnapshot && !vlm_enabled_ && state_.model!=ModelState::Ready)return {false,"MODEL_NOT_READY"};
  if (job_ || completion_) return {false, "EXECUTOR_BUSY"};
  const bool live=job.command==TaskCommand::CaptureSnapshot || (job.command==TaskCommand::DetectSnapshot&&job.input.snapshot_id.empty());
  if (!job.task_id || (live&&!job.generation) || now()>=job.deadline_mono_ns) return {false, "INVALID_OR_EXPIRED_JOB"};
  job_ = job; cancel_requested_ = false; state_.task_id = job.task_id;
  state_.phase = ExecutionPhase::WaitingFrame; state_.physical_busy = true;
  state_.gpu_submitted_mono_ns = 0; ++state_.submitted_jobs; wake_.notify_one(); return {true, ""};
}
ExecutionSnapshot UpstreamExecutor::query() const { std::lock_guard<std::mutex> lock(mu_); return state_; }
std::optional<ExecutionCompletion> UpstreamExecutor::collect() {
  std::lock_guard<std::mutex> lock(mu_);
  auto result = std::move(completion_); completion_.reset();
  if (result) { state_.task_id.reset(); if (!state_.stopped) state_.phase = ExecutionPhase::Idle; }
  return result;
}
void UpstreamExecutor::cancel_pending(uint64_t id) noexcept {
  std::lock_guard<std::mutex> lock(mu_); if (job_ && job_->task_id==id) cancel_requested_ = true; wake_.notify_one();
}
void UpstreamExecutor::shutdown() noexcept {
  std::lock_guard<std::mutex> lock(mu_); closing_ = true; state_.accepting = false; cancel_requested_ = true; wake_.notify_one();
}
void UpstreamExecutor::worker_loop() {
  std::unique_ptr<UpstreamDetector> detector;
  InferenceConfig config;
  bool detector_context_owned=false;
#ifdef EDGEVISION_HAS_AGENT_BACKEND
  std::unique_ptr<AgentTools> tools;
#endif
  auto canceled=[&]{std::lock_guard<std::mutex> lock(mu_);return cancel_requested_||closing_;};
  auto ensure_detector=[&]{
#ifdef EDGEVISION_HAS_AGENT_BACKEND
    if(tools)tools->stop_model(false); // reap before allocating the other model
#endif
    if(detector)return;
    {std::lock_guard<std::mutex> lock(mu_);state_.model=ModelState::Loading;
      state_.vlm_model=ModelState::Unloaded;state_.active_backend="detector_loading";}
    detector_context_owned=true;
    auto candidate=std::make_unique<UpstreamDetector>();candidate->initialize(config.onnx,config.labels,config.kind,config.preprocess);
    detector=std::move(candidate);
    {std::lock_guard<std::mutex> lock(mu_);state_.model=ModelState::Ready;state_.error.clear();
      state_.detector_ready_mono_ns=now();++state_.model_switches;state_.active_backend.clear();}
  };
  try {
    { std::unique_lock<std::mutex> lock(mu_); wake_.wait(lock,[&]{return closing_ || bool(config_);});
      if (!closing_) config = *config_; }
    if (!config.onnx.empty()) {
#ifdef EDGEVISION_HAS_AGENT_BACKEND
      if(agent_enabled_){
        try{tools=std::make_unique<AgentTools>(config.agent);}
        catch(const std::exception& e){std::lock_guard<std::mutex> lock(mu_);state_.vlm_error=std::string(e.what()).substr(0,128);}
      }
#endif
      try{ensure_detector();}
      catch(const std::exception& e){
        if(!agent_enabled_)throw;
        std::lock_guard<std::mutex> lock(mu_);state_.model=ModelState::Error;state_.error=std::string(e.what()).substr(0,128);
      }
      { std::lock_guard<std::mutex> lock(mu_);state_.accepting=!closing_; }
      while (true) {
        ExecutionJob job;bool have_job=false,can_maintain=false;
        { std::unique_lock<std::mutex> lock(mu_);
          if(agent_enabled_)wake_.wait_for(lock,std::chrono::milliseconds(50),[&]{return closing_||bool(job_);});
          else wake_.wait(lock,[&]{return closing_||bool(job_);});
          if(closing_&&!job_)break;
          if(job_){job=*job_;have_job=true;}else can_maintain=!completion_;
        }
        if(!have_job){
#ifdef EDGEVISION_HAS_AGENT_BACKEND
          if(can_maintain&&tools&&tools->model_alive()&&now()-tools->last_activity()>=uint64_t(config.agent.vlm_idle_restore_ms)*1000000){
            {std::lock_guard<std::mutex> lock(mu_);state_.physical_busy=true;state_.phase=ExecutionPhase::Maintenance;}
            try{ensure_detector();}
            catch(const std::exception& e){std::lock_guard<std::mutex> lock(mu_);state_.model=ModelState::Error;state_.error=std::string(e.what()).substr(0,128);}
            {std::lock_guard<std::mutex> lock(mu_);state_.physical_busy=bool(job_);
              state_.phase=job_?ExecutionPhase::WaitingFrame:ExecutionPhase::Idle;state_.accepting=!closing_;}
          }
#else
          (void)can_maintain;
#endif
          continue;
        }
        ExecutionCompletion completion; completion.task_id = job.task_id; completion.generation = job.generation;
        bool detector_operation=false,model_operation=false;
        try {
          if(canceled()||now()>=job.deadline_mono_ns)throw std::runtime_error("EXECUTION_CANCELED_OR_DEADLINE");
          if(job.command==TaskCommand::PlanTool||job.command==TaskCommand::DescribeSnapshot){
#ifdef EDGEVISION_HAS_AGENT_BACKEND
            if(!tools)throw std::runtime_error("AGENT_BACKEND_UNAVAILABLE");
            if(job.command==TaskCommand::DescribeSnapshot)tools->query(job.input.snapshot_id,job.generation,now());
            if(detector){detector.reset();std::lock_guard<std::mutex> lock(mu_);state_.detector_unloaded_mono_ns=now();++state_.model_switches;}
            if(detector_context_owned){
              // This is the sole CUDA owner in this process. All prior jobs
              // have physically completed, and every detector/event/stream
              // object has been destroyed above. Release the retained primary
              // context before starting the separate VLM process; capture and
              // ROS image work are CPU-only. Never reset an in-flight context.
              detector_operation=true;
              const auto released=cudaDeviceReset();
              if(released!=cudaSuccess){
                std::lock_guard<std::mutex> lock(mu_);state_.accepting=false;
                throw std::runtime_error(std::string("CUDA_CONTEXT_RELEASE_FAILED:")+cudaGetErrorString(released));
              }
              detector_context_owned=false;detector_operation=false;
#ifdef __GLIBC__
              malloc_trim(0);
#endif
              std::lock_guard<std::mutex> lock(mu_);++state_.cuda_context_releases;
            }
            {std::lock_guard<std::mutex> lock(mu_);state_.model=ModelState::Unloaded;state_.vlm_model=ModelState::Loading;
              state_.active_backend="local_vlm";state_.phase=ExecutionPhase::InFlight;}
            model_operation=true;completion=tools->model_task(job,canceled,[&]{
              std::lock_guard<std::mutex> lock(mu_);state_.vlm_model=ModelState::Busy;state_.vlm_requests=tools->model_requests();});model_operation=false;
            {std::lock_guard<std::mutex> lock(mu_);state_.vlm_model=ModelState::Ready;state_.vlm_error.clear();state_.vlm_requests=tools->model_requests();}
#else
            throw std::runtime_error("AGENT_BACKEND_NOT_BUILT");
#endif
          }else if(job.command==TaskCommand::ResumeDetector){
            detector_operation=true;ensure_detector();detector_operation=false;completion.success=true;
            completion.result="{\"schema_version\":1,\"task_id\":"+std::to_string(job.task_id)+",\"generation\":"+std::to_string(job.generation)+",\"detector_ready\":true}";
          }else{
            FrameLease frame;
            const bool stored=!job.input.snapshot_id.empty();
            if(stored){
#ifdef EDGEVISION_HAS_AGENT_BACKEND
              if(!tools)throw std::runtime_error("SNAPSHOT_BACKEND_UNAVAILABLE");
              frame=tools->replay(job.input.snapshot_id,job.generation,now());
#else
              throw std::runtime_error("AGENT_BACKEND_NOT_BUILT");
#endif
            }
            if(job.command==TaskCommand::DetectSnapshot){detector_operation=true;ensure_detector();detector_operation=false;}
            if(!stored){
              const uint64_t max_age=uint64_t(job.command==TaskCommand::CaptureSnapshot?job.input.freshness_ms:config.max_frame_age_ms)*1000000;
              while(!canceled()&&now()<job.deadline_mono_ns){
                if(!capture_.consume(frame,5))continue;const auto at=now();
                if(frame->session_generation!=job.generation||at<frame->host_dequeue_mono_ns||at-frame->host_dequeue_mono_ns>max_age){frame.release();continue;}
                break;
              }
            }
            if(canceled()||now()>=job.deadline_mono_ns||!frame.valid())throw std::runtime_error("NO_FRESH_FRAME_BEFORE_DEADLINE");
            if(job.command==TaskCommand::CaptureSnapshot){
#ifdef EDGEVISION_HAS_AGENT_BACKEND
              if(!tools)throw std::runtime_error("SNAPSHOT_BACKEND_UNAVAILABLE");
              completion=tools->snapshot_result(job,*frame);
#else
              throw std::runtime_error("AGENT_BACKEND_NOT_BUILT");
#endif
            }else{
              if(health_&&!stored)health_->observe_consumed(*frame,now());
              if(inference_preview_)inference_preview_->offer(*frame);
              detector_operation=true;
              auto result=detector->infer(std::move(frame),[&](const FrameLease& in_flight){
                if(acceptance_gpu_work_)acceptance_gpu_work_();
                std::lock_guard<std::mutex> lock(mu_);state_.model=ModelState::Busy;state_.phase=ExecutionPhase::InFlight;state_.active_backend="detector";
                state_.held_frame_id=in_flight->frame_id;state_.held_slot_state=in_flight.state();state_.gpu_submitted_mono_ns=now();++state_.gpu_submissions;
              });
              detector_operation=false;
              completion.frame=result.frame;completion.completed_mono_ns=result.completed_mono_ns;
              completion.result=serialize(job.task_id,result,job.input.snapshot_id);completion.success=true;
              if(health_&&!stored)health_->observe_result(result.frame,result.completed_mono_ns);
              {std::lock_guard<std::mutex> lock(mu_);++state_.gpu_completions;}
            }
          }
        } catch (const std::exception& e) {
          completion.success = false; completion.result.clear(); completion.error = std::string(e.what()).substr(0,128);
          if(model_operation){
#ifdef EDGEVISION_HAS_AGENT_BACKEND
            if(tools)tools->stop_model(true);
#endif
            const bool expected_cancel=canceled()||now()>=job.deadline_mono_ns;
            std::lock_guard<std::mutex> lock(mu_);state_.vlm_model=expected_cancel?ModelState::Unloaded:ModelState::Error;
            state_.vlm_error=expected_cancel?"":completion.error;
#ifdef EDGEVISION_HAS_AGENT_BACKEND
            if(tools)state_.vlm_requests=tools->model_requests();
#endif
          }else if(detector_operation){
            detector.reset();std::lock_guard<std::mutex> lock(mu_);state_.model=ModelState::Error;state_.error=completion.error;
            if(!agent_enabled_)state_.accepting=false;
          }
        }
        if (!completion.completed_mono_ns) completion.completed_mono_ns = now();
        { std::lock_guard<std::mutex> lock(mu_);
          // infer() and every local lease have finished before publication.
          completion_ = std::move(completion); job_.reset(); cancel_requested_ = false;
          state_.physical_busy = false; state_.held_frame_id.reset(); state_.held_slot_state = SlotState::Free;
          state_.phase = ExecutionPhase::CompletionReady;state_.active_backend.clear();
          if (state_.model!=ModelState::Error) state_.model = detector?ModelState::Ready:ModelState::Unloaded;
          if (closing_ || (!agent_enabled_&&state_.model==ModelState::Error)) break;
        }
      }
    }
  } catch (const std::exception& e) {
    std::lock_guard<std::mutex> lock(mu_); state_.model = ModelState::Error; state_.error = std::string(e.what()).substr(0,128);
  }
#ifdef EDGEVISION_HAS_AGENT_BACKEND
  tools.reset(); // child is physically reaped before stopped is acknowledged
#endif
  detector.reset(); // model, context and buffers are destroyed by their GPU owner
  { std::lock_guard<std::mutex> lock(mu_); state_.accepting = false; state_.stopped = true; state_.physical_busy = false;
    state_.phase = ExecutionPhase::Stopped;state_.vlm_model=ModelState::Unloaded;state_.active_backend.clear();
    if (state_.model!=ModelState::Error) state_.model = ModelState::Unloaded; }
}
} // namespace edgevision
