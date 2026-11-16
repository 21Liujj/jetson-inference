#include "edgevision/upstream_executor.h"
#include "edgevision/upstream_detector.h"
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
std::string serialize(uint64_t task, const DetectionResult& result) {
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
ExecutionReply UpstreamExecutor::initialize(InferenceConfig config) {
  std::lock_guard<std::mutex> lock(mu_);
  if (closing_ || config_) return {false, "MODEL_ALREADY_INITIALIZED_OR_CLOSING"};
  if (config.onnx.empty() || config.labels.empty() || config.onnx.size()>512 || config.labels.size()>512 ||
      config.onnx.find('\0')!=std::string::npos || config.labels.find('\0')!=std::string::npos ||
      !config.max_frame_age_ms || config.max_frame_age_ms>5000 ||
      (config.kind!=DetectorKind::SsdFruit && config.kind!=DetectorKind::YoloV8Coco) ||
      (config.preprocess!=PreprocessMode::CpuBaseline && config.preprocess!=PreprocessMode::CudaFused) ||
      (config.preprocess==PreprocessMode::CudaFused && config.kind!=DetectorKind::YoloV8Coco)) return {false, "INVALID_MODEL_CONFIG"};
  config_ = std::move(config); state_.model = ModelState::Loading; wake_.notify_one(); return {true, ""};
}
ExecutionReply UpstreamExecutor::submit(ExecutionJob job) {
  std::lock_guard<std::mutex> lock(mu_);
  if (closing_ || !state_.accepting || state_.model!=ModelState::Ready) return {false, "MODEL_NOT_READY"};
  if (job_ || completion_) return {false, "EXECUTOR_BUSY"};
  if (!job.task_id || !job.generation || now()>=job.deadline_mono_ns) return {false, "INVALID_OR_EXPIRED_JOB"};
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
  try {
    InferenceConfig config;
    { std::unique_lock<std::mutex> lock(mu_); wake_.wait(lock,[&]{return closing_ || bool(config_);});
      if (!closing_) config = *config_; }
    if (!config.onnx.empty()) {
      detector.reset(new UpstreamDetector); detector->initialize(config.onnx, config.labels, config.kind,config.preprocess);
      { std::lock_guard<std::mutex> lock(mu_); state_.model = ModelState::Ready; state_.accepting = !closing_; }
      while (true) {
        ExecutionJob job;
        { std::unique_lock<std::mutex> lock(mu_); wake_.wait(lock,[&]{return closing_ || bool(job_);});
          if (!job_) break;
          job = *job_; }
        ExecutionCompletion completion; completion.task_id = job.task_id; completion.generation = job.generation;
        try {
          FrameLease frame;
          while (true) {
            { std::lock_guard<std::mutex> lock(mu_);
              if (cancel_requested_ || closing_) { completion.error = "CANCELED_BEFORE_GPU"; break; } }
            if (now()>=job.deadline_mono_ns) { completion.error = "NO_FRESH_FRAME_BEFORE_DEADLINE"; break; }
            if (!capture_.consume(frame,5)) continue;
            const auto at = now();
            if (frame->session_generation!=job.generation || at<frame->host_dequeue_mono_ns ||
                at-frame->host_dequeue_mono_ns>uint64_t(config.max_frame_age_ms)*1000000) { frame.release(); continue; }
            bool skip;
            { std::lock_guard<std::mutex> lock(mu_); skip = cancel_requested_ || closing_ || at>=job.deadline_mono_ns; }
            if (skip) { frame.release(); continue; }
            if (health_) health_->observe_consumed(*frame,at);
            if (inference_preview_) inference_preview_->offer(*frame);
            auto result = detector->infer(std::move(frame),[&](const FrameLease& in_flight) {
              if (acceptance_gpu_work_) acceptance_gpu_work_(); // explicit acceptance injection only
              std::lock_guard<std::mutex> lock(mu_);
              state_.model = ModelState::Busy; state_.phase = ExecutionPhase::InFlight;
              state_.held_frame_id = in_flight->frame_id; state_.held_slot_state = in_flight.state();
              state_.gpu_submitted_mono_ns = now(); ++state_.gpu_submissions;
            });
            completion.frame = result.frame; completion.completed_mono_ns = result.completed_mono_ns;
            completion.result = serialize(job.task_id,result); completion.success = true;
            if (health_) health_->observe_result(result.frame,result.completed_mono_ns);
            { std::lock_guard<std::mutex> lock(mu_); ++state_.gpu_completions; }
            break;
          }
        } catch (const std::exception& e) {
          completion.success = false; completion.result.clear(); completion.error = std::string(e.what()).substr(0,128);
          std::lock_guard<std::mutex> lock(mu_); state_.model = ModelState::Error; state_.error = completion.error; state_.accepting = false;
        }
        if (!completion.completed_mono_ns) completion.completed_mono_ns = now();
        { std::lock_guard<std::mutex> lock(mu_);
          // infer() and every local lease have finished before publication.
          completion_ = std::move(completion); job_.reset(); cancel_requested_ = false;
          state_.physical_busy = false; state_.held_frame_id.reset(); state_.held_slot_state = SlotState::Free;
          state_.phase = ExecutionPhase::CompletionReady;
          if (state_.model!=ModelState::Error) state_.model = ModelState::Ready;
          if (closing_ || state_.model==ModelState::Error) break;
        }
      }
    }
  } catch (const std::exception& e) {
    std::lock_guard<std::mutex> lock(mu_); state_.model = ModelState::Error; state_.error = std::string(e.what()).substr(0,128);
  }
  detector.reset(); // model, context and buffers are destroyed by their GPU owner
  { std::lock_guard<std::mutex> lock(mu_); state_.accepting = false; state_.stopped = true; state_.physical_busy = false;
    state_.phase = ExecutionPhase::Stopped; if (state_.model!=ModelState::Error) state_.model = ModelState::Unloaded; }
}
} // namespace edgevision
