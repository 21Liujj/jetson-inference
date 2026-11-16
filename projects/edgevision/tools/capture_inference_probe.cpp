// Real direct-V4L2 -> repaired upstream CUDA/DetectNet baseline. No images saved.
#include "edgevision/upstream_detector.h"
#include "edgevision/v4l2_backend.h"
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <thread>
using namespace edgevision;
namespace {
volatile std::sig_atomic_t interrupted = 0;
void signal_handler(int) { interrupted = 1; }
uint64_t now() { return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
std::string quote(const std::string& s) {
  std::string out = "\"";
  for (unsigned char c : s) { if (c == '"' || c == '\\') out += '\\'; out += c < 32 ? '?' : char(c); }
  return out + '"';
}
const char* name(CaptureState s) {
  switch (s) {
    case CaptureState::Idle: return "IDLE"; case CaptureState::Starting: return "STARTING";
    case CaptureState::Running: return "RUNNING"; case CaptureState::Stopping: return "STOPPING";
    case CaptureState::Recovering: return "RECOVERING"; case CaptureState::Error: return "ERROR";
  } return "UNKNOWN";
}
const char* name(ModelState s) {
  switch (s) { case ModelState::Unloaded: return "UNLOADED"; case ModelState::Loading: return "LOADING";
    case ModelState::Ready: return "READY"; case ModelState::Busy: return "BUSY"; case ModelState::Error: return "ERROR"; }
  return "UNKNOWN";
}
template<class T> void optional(std::ostream& s, const std::optional<T>& v) { if (v) s << *v; }
}
int main(int argc, char** argv) {
  try {
    CaptureConfig config; std::string output, model, labels; unsigned seconds = 10;
    DetectorKind kind=DetectorKind::SsdFruit;
    PreprocessMode preprocess=PreprocessMode::CpuBaseline;
    for (int i = 1; i < argc; ++i) {
      if (i + 1 >= argc) throw std::invalid_argument("option needs value");
      const std::string key = argv[i], value = argv[++i];
      if (key == "--device") config.device = value;
      else if (key == "--out") output = value;
      else if (key == "--model") model = value;
      else if (key == "--labels") labels = value;
      else if (key == "--kind") {
        if (value=="ssd") kind=DetectorKind::SsdFruit;
        else if (value=="yolov8") kind=DetectorKind::YoloV8Coco;
        else throw std::invalid_argument("unsupported model kind");
      }
      else if(key=="--preprocess") {
        if(value=="cuda")preprocess=PreprocessMode::CudaFused;
        else if(value!="cpu")throw std::invalid_argument("unsupported preprocessor");
      }
      else if (key == "--seconds") {
        if (value.empty() || value.find_first_not_of("0123456789") != std::string::npos) throw std::invalid_argument("invalid duration");
        const auto n = std::stoul(value); if (!n || n > 1800) throw std::invalid_argument("duration budget"); seconds = n;
      } else throw std::invalid_argument("unknown option");
    }
    if (!ControlRouter::valid_config(config) || output.empty() || model.empty() || labels.empty()) throw std::invalid_argument("invalid config");
    if (!std::filesystem::create_directories(output)) throw std::invalid_argument("output already exists");
    std::ofstream events(output + "/events.jsonl"), results(output + "/detections.jsonl"), metrics(output + "/health.csv");
    for (auto* file : {&events, &results, &metrics}) file->exceptions(std::ios::badbit | std::ios::failbit);
    metrics << "mono_ns,capture,model,generation,valid_capture_frames,queue_depth,pool_busy,pool_drop,ready_drop,completed_inferences,published_results,stale_or_stopped_results,result_window_n,dq_to_result_p95_ns\n";
    V4L2Backend backend; ControlRouter router(backend); HealthMonitor health;
    std::atomic<bool> stop_worker{false}, worker_done{false};
    std::atomic<ModelState> worker_model{ModelState::Loading};
    std::atomic<uint64_t> completed{0}, published{0}, stale{0}, first_result_at{0}, empty_results{0};
    std::mutex error_mu; std::string worker_error;
    const auto started = now();
    auto start = router.submit({"inference-start", 0, started + 10000000000ULL, TaskCommand::StartCapture, config}, started);
    if (!start.accepted) throw std::runtime_error("START_ADMISSION_REJECTED");
    std::signal(SIGINT, signal_handler); std::signal(SIGTERM, signal_handler);
    std::thread worker([&] {
      try {
        UpstreamDetector detector; detector.initialize(model, labels, kind,preprocess);
        worker_model = ModelState::Ready;
        while (!stop_worker) {
          auto status = router.status();
          if (status.capture != CaptureState::Running) { std::this_thread::sleep_for(std::chrono::milliseconds(2)); continue; }
          FrameLease frame;
          if (!backend.consume(frame, 5)) continue;
          status = router.status();
          if (stop_worker || status.capture != CaptureState::Running || frame->session_generation != status.generation) continue;
          health.observe_consumed(*frame, now()); worker_model = ModelState::Busy;
          auto result = detector.infer(std::move(frame)); // owns lease through real CUDA completion
          ++completed; if (result.boxes.empty()) ++empty_results;
          worker_model = ModelState::Ready;
          status = router.status();
          if (stop_worker || status.generation != result.frame.session_generation || status.capture != CaptureState::Running) { ++stale; continue; }
          health.observe_result(result.frame, result.completed_mono_ns);
          results << "{\"generation\":" << result.frame.session_generation << ",\"frame_id\":" << result.frame.frame_id
                  << ",\"sequence\":" << result.frame.driver_sequence << ",\"host_dq_ns\":" << result.frame.host_dequeue_mono_ns
                  << ",\"completed_mono_ns\":" << result.completed_mono_ns << ",\"width\":" << result.frame.width << ",\"height\":" << result.frame.height << ",\"detections\":[";
          for (std::size_t i = 0; i < result.boxes.size(); ++i) {
            if (i) results << ','; const auto& b = result.boxes[i];
            results << "{\"class_id\":" << b.class_id << ",\"label\":" << quote(b.label) << ",\"confidence\":" << b.confidence
                    << ",\"left\":" << b.left << ",\"top\":" << b.top << ",\"right\":" << b.right << ",\"bottom\":" << b.bottom << '}';
          }
          results << "]}\n"; results.flush(); ++published;
          uint64_t zero = 0; first_result_at.compare_exchange_strong(zero, now());
        }
      } catch (const std::exception& e) {
        { std::lock_guard<std::mutex> lock(error_mu); worker_error = e.what(); }
        worker_model = ModelState::Error;
      }
      if (worker_model != ModelState::Error) worker_model = ModelState::Unloaded;
      worker_done = true;
    });
    std::string failure; uint64_t next_metrics = 0, model_failed_at = 0;
    CaptureState previous_capture = CaptureState::Idle; bool have_state = false, model_ready_logged = false;
    auto log = [&](const std::string& event, const RouterStatus& state) {
      events << "{\"mono_ns\":" << now() << ",\"event\":" << quote(event) << ",\"capture\":" << quote(name(state.capture))
             << ",\"model\":" << quote(name(state.model)) << ",\"generation\":" << state.generation
             << ",\"resources_live\":" << (state.backend_resources_live ? "true" : "false")
             << ",\"capture_error\":" << quote(state.last_error) << ",\"model_error\":" << quote(state.model_error) << "}\n";
      events.flush();
    };
    auto observe = [&] {
      std::string error; { std::lock_guard<std::mutex> lock(error_mu); error = worker_error; }
      router.observe_model(worker_model.load(), error); router.poll(now());
    };
    try {
      while (!interrupted) {
        observe(); const auto state = router.status(); const auto at = now();
        if (!have_state || previous_capture != state.capture) { log("capture_state", state); have_state = true; previous_capture = state.capture; }
        if (!model_ready_logged && (state.model == ModelState::Ready || state.model == ModelState::Busy)) { log("model_ready", state); model_ready_logged = true; }
        if (state.model == ModelState::Error) {
          if (!model_failed_at) { model_failed_at = at; log("model_failure_capture_independent", state); }
          if (at - model_failed_at >= 1000000000ULL) { log("model_failure_capture_observed", state); failure = state.model_error; break; }
        }
        if (state.capture == CaptureState::Error) { failure = state.last_error; break; }
        if (at >= next_metrics) {
          health.observe_capture(backend.health_sample(at)); const auto h = health.snapshot(at, state);
          metrics << at << ',' << name(state.capture) << ',' << name(state.model) << ',' << state.generation << ',' << h.counters.valid_frames
                  << ',' << h.counters.queue_depth << ',' << h.counters.pool_busy << ',' << h.counters.pool_drops << ',' << h.counters.ready_drops
                  << ',' << completed << ',' << published << ',' << stale << ',' << h.dequeue_to_result.samples << ',';
          optional(metrics, h.dequeue_to_result.p95_ns); metrics << '\n'; metrics.flush(); next_metrics = at + 250000000;
        }
        const auto first = first_result_at.load();
        if (first && at - first >= uint64_t(seconds) * 1000000000) break;
        if (!first && at - started >= 60000000000ULL) { failure = "NO_INFERENCE_RESULT_IN_STARTUP_BUDGET"; break; }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
    } catch (const std::exception& e) { failure = e.what(); }
    stop_worker = true; // admission ends before capture stop; in-flight infer still completes
    const auto stop_at = now(); const auto pre_stop = router.status();
    const auto stop = router.submit({"inference-stop", pre_stop.generation, stop_at + 12000000000ULL, TaskCommand::StopCapture, {}}, stop_at);
    if (!stop.accepted) { std::fputs("STOP_ADMISSION_REJECTED\n", stderr); std::_Exit(71); }
    while (true) {
      observe();
      if (!router.status().backend_resources_live && worker_done.load()) break;
      if (now() - stop_at >= 15000000000ULL) { std::fputs("CLEANUP_UNKNOWN_PROCESS_ISOLATION\n", stderr); std::fflush(nullptr); std::_Exit(71); }
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    worker.join(); observe(); const auto final = router.status(); const auto capture = backend.snapshot(); log("final", final);
    if (router.query(stop.task_id)->state != TaskState::Done || final.capture != CaptureState::Idle || capture.pool_busy || capture.worker_active) failure = "STOP_NOT_PHYSICALLY_COMPLETED";
    if (failure.empty() && !published) failure = "NO_VALID_INFERENCE_RESULT";
    if (interrupted && failure.empty()) failure = "INTERRUPTED";
    std::ofstream summary(output + "/summary.json");
    summary << "{\"scope\":\"single-flight direct V4L2 to upstream inference; not final-stack endurance\",\"preprocessor\":"
            << quote(preprocess==PreprocessMode::CudaFused?"cuda-fused":"cpu-baseline") << ",\"model_kind\":"
            << quote(kind==DetectorKind::YoloV8Coco?"yolov8-coco":"ssd-fruit") << ",\"completed_inferences\":" << completed
            << ",\"published_results\":" << published << ",\"empty_results\":" << empty_results << ",\"stale_or_stopped_results\":" << stale
            << ",\"copied_frames\":" << capture.io.frames << ",\"pool_drop\":" << capture.io.no_slot << ",\"ready_drop\":" << capture.queue_replaced
            << ",\"sequence_gaps\":" << capture.io.sequence_gaps << ",\"final_pool_busy\":" << capture.pool_busy
            << ",\"stop_ns\":" << now() - stop_at << ",\"pre_stop_capture\":" << quote(name(pre_stop.capture)) << ",\"pre_stop_model\":" << quote(name(pre_stop.model))
            << ",\"failure\":" << quote(failure) << "}\n";
    std::cout << "INFERENCE_PROBE " << (failure.empty() ? "PASS" : "FAIL") << " completed=" << completed << " published=" << published << std::endl;
    return failure.empty() ? 0 : 1;
  } catch (const std::exception& e) { std::cerr << "INFERENCE_PROBE_FAILED: " << e.what() << std::endl; return 1; }
}
