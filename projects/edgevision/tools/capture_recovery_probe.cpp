// Real-device recovery/diagnostics acceptance. Only metadata is persisted.
#include "edgevision/v4l2_backend.h"
#include <chrono>
#include <csignal>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
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
  }
  return "UNKNOWN";
}
ProcessHealthSample process(uint64_t at) {
  ProcessHealthSample s; s.observed_mono_ns = at; std::ifstream status("/proc/self/status"); std::string line;
  while (std::getline(status, line)) if (line.rfind("VmRSS:", 0) == 0) {
    uint64_t n; if (std::istringstream(line.substr(6)) >> n) s.rss_kib = n; break;
  }
  try { uint64_t n = 0; for (auto& entry : std::filesystem::directory_iterator("/proc/self/fd")) { (void)entry; ++n; } s.fd_count = n; }
  catch (const std::filesystem::filesystem_error&) {}
  return s;
}
template<class T> void optional(std::ostream& out, const std::optional<T>& value) { if (value) out << *value; }
}
int main(int argc, char** argv) {
  try {
    CaptureConfig config; config.runtime_retry_limit = 5; config.runtime_recovery_budget_ms = 30000;
    std::string output; unsigned seconds = 90; bool stop_during = false, require_recovery = true;
    for (int i = 1; i < argc; ++i) {
      std::string key = argv[i];
      if (key == "--stop-during-recovery") { stop_during = true; continue; }
      if (key == "--no-recovery-required") { require_recovery = false; continue; }
      if (i + 1 >= argc) throw std::invalid_argument("option needs value");
      std::string value = argv[++i];
      if (key == "--device") config.device = value;
      else if (key == "--out") output = value;
      else if (key == "--seconds") {
        if (value.empty() || value.find_first_not_of("0123456789") != std::string::npos) throw std::invalid_argument("invalid duration");
        auto n = std::stoul(value); if (n < 3 || n > 600) throw std::invalid_argument("duration budget"); seconds = n;
      } else throw std::invalid_argument("unknown option");
    }
    if (!ControlRouter::valid_config(config) || output.empty()) throw std::invalid_argument("invalid capture config/output");
    if (!std::filesystem::create_directories(output)) throw std::invalid_argument("output already exists");
    std::ofstream events(output + "/events.jsonl"), metrics(output + "/health.csv");
    events.exceptions(std::ios::badbit | std::ios::failbit); metrics.exceptions(std::ios::badbit | std::ios::failbit);
    metrics << "mono_ns,state,generation,valid_frames,pool_drop,ready_drop,depth,pool_busy,rate_n,rate_interval_ns,capture_fps,consume_n,consume_p95_ns,result_n,result_p95_ns,frame_age_from_dq_ns,rss_kib,fd_count\n";
    auto log = [&](const std::string& what, const RouterStatus& s) {
      events << "{\"mono_ns\":" << now() << ",\"event\":" << quote(what) << ",\"state\":" << quote(name(s.capture))
             << ",\"generation\":" << s.generation << ",\"runtime_retries\":" << s.runtime_retries
             << ",\"resources_live\":" << (s.backend_resources_live ? "true" : "false") << ",\"error\":" << quote(s.last_error) << "}\n";
      events.flush(); std::cout << what << " state=" << name(s.capture) << " generation=" << s.generation << std::endl;
    };
    V4L2Backend backend; ControlRouter router(backend); HealthMonitor health;
    std::signal(SIGINT, signal_handler); std::signal(SIGTERM, signal_handler);
    auto start_at = now();
    auto task = router.submit({"recovery-start", 0, start_at + 10000000000ULL, TaskCommand::StartCapture, config}, start_at);
    if (!task.accepted) throw std::runtime_error("START_ADMISSION_REJECTED");
    RouterStatus previous; bool have_previous = false, ready = false, recovered = false, stopped_in_recovery = false;
    uint64_t frames = 0, first_generation = 0, next_metrics = 0, recovery_observed_at = 0;
    uint64_t health_generation = 0;
    std::string failure;
    while (!interrupted && now() - start_at < uint64_t(seconds) * 1000000000) {
      const auto at = now(); router.poll(at); auto state = router.status();
      if (!have_previous || state.capture != previous.capture || state.generation != previous.generation) log("state", state);
      previous = state; have_previous = true;
      if (state.capture == CaptureState::Running && !ready) { ready = true; first_generation = state.generation; log("ready_for_disconnect", state); }
      if (ready && (state.capture == CaptureState::Recovering || state.capture == CaptureState::Stopping)) {
        if (!recovery_observed_at) recovery_observed_at = at;
        if (stop_during) { stopped_in_recovery = true; break; }
      }
      if (state.capture == CaptureState::Error && !state.backend_resources_live) { failure = state.last_error; break; }
      const bool metrics_due = at >= next_metrics;
      if (state.generation && (metrics_due || state.generation != health_generation)) {
        const auto sample = backend.health_sample(at); health.observe_capture(sample); health_generation = sample.generation;
      }
      if (metrics_due) next_metrics = at + 250000000;
      FrameLease frame;
      if (state.capture == CaptureState::Running && backend.consume(frame, 0)) {
        if (frame->session_generation == state.generation) {
          ++frames; health.observe_consumed(*frame, now());
          if (ready && frame->session_generation != first_generation && !recovered) {
            recovered = true; recovery_observed_at = now(); log("recovered_valid_frame", state);
          }
        }
      }
      if (metrics_due) {
        health.observe_process(process(now())); auto h = health.snapshot(now(), state);
        metrics << at << ',' << name(state.capture) << ',' << h.generation << ',' << h.counters.valid_frames << ',' << h.counters.pool_drops << ','
                << h.counters.ready_drops << ',' << h.counters.queue_depth << ',' << h.counters.pool_busy << ',' << h.rate.samples << ',' << h.rate.interval_ns << ',';
        optional(metrics, h.rate.valid_capture_fps); metrics << ',' << h.dequeue_to_consume.samples << ','; optional(metrics, h.dequeue_to_consume.p95_ns);
        metrics << ',' << h.dequeue_to_result.samples << ','; optional(metrics, h.dequeue_to_result.p95_ns);
        metrics << ','; optional(metrics, h.frame_age_from_dequeue_ns); metrics << ',';
        if (h.process) optional(metrics, h.process->rss_kib); metrics << ',';
        if (h.process) optional(metrics, h.process->fd_count); metrics << '\n'; metrics.flush();
      }
      if (recovered && now() - recovery_observed_at >= 2000000000ULL) break; // observe continued delivery after recovery
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    auto stop_at = now(); auto before_stop = router.status();
    auto stop = router.submit({"recovery-stop", before_stop.generation, stop_at + 12000000000ULL, TaskCommand::StopCapture, {}}, stop_at);
    if (!stop.accepted) throw std::runtime_error("STOP_ADMISSION_REJECTED");
    log(stopped_in_recovery ? "stop_requested_during_recovery" : "stop_requested", before_stop);
    do { router.poll(now()); if (!router.status().backend_resources_live && !router.query(stop.task_id)->execution_pending &&
            router.query(stop.task_id)->state != TaskState::Queued) break;
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    } while (now() - stop_at < 15000000000ULL);
    auto final = router.status(); log("cleanup_complete", final);
    if (final.backend_resources_live || backend.snapshot().pool_busy) failure = "CLEANUP_PENDING";
    if (router.query(stop.task_id)->state != TaskState::Done || final.capture != CaptureState::Idle) failure = "STOP_NOT_COMPLETED";
    bool replug_while_idle = false;
    uint64_t stop_observation_ns = 0;
    if (stopped_in_recovery) {
      const auto generation = final.generation;
      const auto observation_start = now();
      bool absent = !std::filesystem::exists(config.device);
      uint64_t returned_at = 0;
      while (now() - observation_start < 60000000000ULL && !interrupted) {
        router.poll(now()); const auto state = router.status();
        if (state.generation != generation || state.capture != CaptureState::Idle || state.backend_resources_live || backend.snapshot().worker_active) {
          failure = "REOPEN_AFTER_EXPLICIT_STOP"; break;
        }
        const bool present = std::filesystem::exists(config.device);
        if (!present) absent = true;
        if (absent && present && !returned_at) { returned_at = now(); log("camera_node_returned_while_idle", state); }
        if (returned_at && now() - returned_at >= 5000000000ULL) { replug_while_idle = true; break; }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
      }
      stop_observation_ns = now() - observation_start;
      if (!replug_while_idle && failure.empty()) failure = "POST_STOP_REPLUG_NOT_OBSERVED";
    }
    final = router.status(); log("final", final);
    if (!ready || !frames) failure = "NO_VALID_INITIAL_FRAME";
    if (require_recovery && !recovered && !stopped_in_recovery && failure.empty()) failure = "RECOVERY_NOT_OBSERVED";
    if (interrupted && failure.empty()) failure = "INTERRUPTED";
    std::ofstream summary(output + "/summary.json");
    summary << "{\"scope\":\"real USB capture runtime recovery; no inference\",\"frames\":" << frames
            << ",\"recovered\":" << (recovered ? "true" : "false") << ",\"stopped_in_recovery\":" << (stopped_in_recovery ? "true" : "false")
            << ",\"generation\":" << final.generation << ",\"runtime_retries\":" << final.runtime_retries << ",\"fault_detected_ns\":" << final.fault_detected_mono_ns
            << ",\"recovered_ns\":" << final.recovered_mono_ns << ",\"runtime_retry_limit\":5,\"recovery_budget_ms\":30000"
            << ",\"post_stop_node_returned_while_idle\":" << (replug_while_idle ? "true" : "false")
            << ",\"post_stop_observation_ns\":" << stop_observation_ns << ",\"failure\":" << quote(failure) << "}\n";
    return failure.empty() ? 0 : 1;
  } catch (const std::exception& e) { std::cerr << "RECOVERY_PROBE_FAILED: " << e.what() << std::endl; return 1; }
}
