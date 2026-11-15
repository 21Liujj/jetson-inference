// Real USB/MMAP + common ControlRouter acceptance tool. Metadata only: no image
// files, TensorRT or CUDA. Explicit slow/held consumers are test injections.
#include "edgevision/v4l2_backend.h"
#include <csignal>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <chrono>
#include <thread>
using namespace edgevision;
namespace {
volatile std::sig_atomic_t interrupted = 0;
void signal_handler(int) { interrupted = 1; }
uint64_t now() { return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
std::string quote(const std::string& s) {
  std::string out = "\"";
  for (unsigned char c : s) {
    if (c == '"' || c == '\\') { out += '\\'; out += char(c); }
    else if (c < 32) out += '?'; else out += char(c);
  }
  return out + '"';
}
std::size_t fds() { std::size_t n = 0; for (auto& entry : std::filesystem::directory_iterator("/proc/self/fd")) { (void)entry; ++n; } return n; }
uint64_t rss_kib() {
  std::ifstream f("/proc/self/status"); std::string s;
  while (std::getline(f, s)) if (s.rfind("VmRSS:", 0) == 0) { uint64_t n; std::istringstream(s.substr(6)) >> n; return n; }
  return 0;
}
std::size_t video_maps() {
  std::ifstream f("/proc/self/maps"); std::string s; std::size_t n = 0;
  while (std::getline(f, s)) if (s.find("/dev/video") != std::string::npos) ++n;
  return n;
}
template<class Done> void pump(ControlRouter& router, Done done) {
  const auto limit = now() + 15000000000ULL;
  do { router.poll(now()); if (done()) return; std::this_thread::sleep_for(std::chrono::milliseconds(1)); } while (now() < limit);
  throw std::runtime_error("CONTROL_CLEANUP_TIMEOUT");
}
}
int main(int argc, char** argv) {
  try {
    CaptureConfig config; std::string output;
    unsigned seconds = 10, cycles = 1, frames_limit = 0, consumer_ms = 0, hold_slots = 0;
    for (int i = 1; i < argc; ++i) {
      std::string key = argv[i];
      if (key == "--allow-unstable-device") { config.allow_unstable_device = true; continue; }
      if (i + 1 >= argc) throw std::invalid_argument("option needs value");
      std::string value = argv[++i];
      if (key == "--device") config.device = value;
      else if (key == "--out") output = value;
      else if (key == "--format") {
        if (value != "YUYV" && value != "MJPG") throw std::invalid_argument("unsupported format");
        config.fourcc = make_fourcc(value[0], value[1], value[2], value[3]);
      } else {
        if (value.empty() || value.find_first_not_of("0123456789") != std::string::npos) throw std::invalid_argument("invalid number");
        const auto n = std::stoull(value); if (n > 1000000) throw std::invalid_argument("option budget exceeded");
        if (key == "--seconds") seconds = n; else if (key == "--cycles") cycles = n;
        else if (key == "--frames") frames_limit = n; else if (key == "--consumer-ms") consumer_ms = n;
        else if (key == "--hold-slots") hold_slots = n;
        else if (key == "--width") config.width = n; else if (key == "--height") config.height = n;
        else throw std::invalid_argument("unknown option");
      }
    }
    if (!ControlRouter::valid_config(config) || output.empty() || !seconds || seconds > 1800 || !cycles || cycles > 100 ||
        consumer_ms > 1000 || hold_slots > config.pool_slots) throw std::invalid_argument("invalid config/run budget");
    if (!std::filesystem::create_directories(output)) throw std::invalid_argument("output directory already exists");
    std::signal(SIGINT, signal_handler); std::signal(SIGTERM, signal_handler);
    std::ofstream frame_log(output + "/frames.csv"), cycle_log(output + "/cycles.csv");
    frame_log.exceptions(std::ios::badbit | std::ios::failbit); cycle_log.exceptions(std::ios::badbit | std::ios::failbit);
    frame_log << "cycle,generation,frame_id,sequence,width,height,fourcc,stride,bytesused,flags,driver_ns,driver_clock,driver_source,host_dq_ns,publish_ns,consume_ns\n";
    cycle_log << "cycle,generation,consumed,dequeued,copied,pool_drop,invalid,sequence_gaps,queue_replaced,queue_aborted,queue_consumed,queue_high_water,fd,rss_kib,video_maps,stop_ns,held_stop_guarded,start_retries\n";
    V4L2Backend backend; ControlRouter router(backend);
    const auto baseline_fds = fds(); unsigned completed = 0;
    uint64_t total_consumed = 0, total_copied = 0, total_pool_drop = 0, total_replaced = 0, total_gaps = 0;
    uint64_t total_start_retries = 0;
    CaptureSnapshot last;
    for (unsigned cycle = 0; cycle < cycles && !interrupted; ++cycle) {
      auto generation = router.status().generation;
      auto start = router.submit({"start-" + std::to_string(cycle), generation, now() + 10000000000ULL, TaskCommand::StartCapture, config}, now());
      if (!start.accepted) throw std::runtime_error("START_ADMISSION_REJECTED");
      pump(router, [&] { auto t = router.query(start.task_id); return t && t->state != TaskState::Queued && t->state != TaskState::Running && !t->execution_pending; });
      if (router.query(start.task_id)->state != TaskState::Done || router.status().capture != CaptureState::Running)
        throw std::runtime_error(router.status().last_error);
      generation = router.status().generation;
      const auto start_retries = router.status().start_retries;
      auto run_start = now(), next_consume = run_start; uint64_t consumed = 0;
      std::vector<FrameLease> held; held.reserve(hold_slots);
      while (!interrupted && now() - run_start < uint64_t(seconds) * 1000000000 && (!frames_limit || consumed < frames_limit)) {
        router.poll(now());
        if (router.status().capture != CaptureState::Running) throw std::runtime_error(router.status().last_error);
        if (now() >= next_consume && (!hold_slots || held.size() < hold_slots)) {
          FrameLease frame;
          if (backend.consume(frame, 1)) {
            const auto& m = *frame; const auto consume = now();
            frame_log << cycle << ',' << m.session_generation << ',' << m.frame_id << ',' << m.driver_sequence << ',' << m.width << ',' << m.height << ','
                      << m.pixfmt_fourcc << ',' << m.stride << ',' << m.bytesused << ',' << m.driver_flags << ',';
            if (m.driver_timestamp_ns) frame_log << *m.driver_timestamp_ns;
            frame_log << ',' << int(m.driver_timestamp_clock) << ',' << int(m.driver_timestamp_source) << ',' << m.host_dequeue_mono_ns << ',' << m.publish_mono_ns << ',' << consume << '\n';
            ++consumed;
            if (hold_slots) held.push_back(std::move(frame));
          }
          next_consume = now() + uint64_t(consumer_ms) * 1000000;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
      const auto stop_start = now();
      auto stop = router.submit({"stop-" + std::to_string(cycle), generation, stop_start + 10000000000ULL, TaskCommand::StopCapture, {}}, stop_start);
      if (!stop.accepted) throw std::runtime_error("STOP_ADMISSION_REJECTED");
      router.poll(now());
      bool held_stop_guarded = false;
      if (!held.empty()) {
        const auto before_release = now() + 50000000;
        while (now() < before_release) { router.poll(now()); std::this_thread::sleep_for(std::chrono::milliseconds(1)); }
        held_stop_guarded = router.query(stop.task_id)->execution_pending && router.status().capture == CaptureState::Stopping;
        if (!held_stop_guarded) throw std::runtime_error("STOP_RELEASED_HELD_STORAGE");
      }
      held.clear();
      pump(router, [&] { return !router.status().backend_resources_live; });
      if (router.query(stop.task_id)->state != TaskState::Done || router.status().capture != CaptureState::Idle) throw std::runtime_error(router.status().last_error);
      last = backend.snapshot();
      if (last.queue_enqueued != last.queue_consumed + last.queue_replaced + last.queue_aborted || last.queue_depth || last.pool_busy ||
          last.io.frames != last.queue_enqueued || last.queue_consumed != consumed || last.io.dequeued != last.io.frames + last.io.no_slot + last.io.invalid_payloads)
        throw std::runtime_error("FRAME_ACCOUNTING_MISMATCH");
      const auto fd = fds(), maps = video_maps();
      if (fd != baseline_fds || maps) throw std::runtime_error("RESOURCE_COUNT_MISMATCH");
      cycle_log << cycle << ',' << generation << ',' << consumed << ',' << last.io.dequeued << ',' << last.io.frames << ',' << last.io.no_slot << ',' << last.io.invalid_payloads << ','
                << last.io.sequence_gaps << ',' << last.queue_replaced << ',' << last.queue_aborted << ',' << last.queue_consumed << ',' << last.queue_high_water << ','
                << fd << ',' << rss_kib() << ',' << maps << ',' << now() - stop_start << ',' << held_stop_guarded << ',' << start_retries << '\n';
      cycle_log.flush(); ++completed; total_consumed += consumed; total_copied += last.io.frames;
      total_pool_drop += last.io.no_slot; total_replaced += last.queue_replaced; total_gaps += last.io.sequence_gaps;
      total_start_retries += start_retries;
      std::cout << "cycle=" << cycle << " generation=" << generation << " frames=" << consumed << " copied=" << last.io.frames << " replaced=" << last.queue_replaced << " fd=" << fd << " maps=" << maps << std::endl;
    }
    frame_log.flush();
    std::ofstream summary(output + "/summary.json");
    summary << "{\n  \"scope\":\"real USB V4L2/MMAP and ControlRouter; no inference\",\n  \"device\":" << quote(config.device)
            << ",\n  \"cycles_completed\":" << completed << ",\n  \"interrupted\":" << (interrupted ? "true" : "false")
            << ",\n  \"consumed\":" << total_consumed << ",\n  \"copied\":" << total_copied << ",\n  \"pool_drop\":" << total_pool_drop
            << ",\n  \"queue_replaced\":" << total_replaced << ",\n  \"sequence_gaps\":" << total_gaps << ",\n  \"start_retries\":" << total_start_retries
            << ",\n  \"negotiated\":{\"width\":" << last.format.width << ",\"height\":" << last.format.height << ",\"fourcc\":" << last.format.fourcc << ",\"stride\":" << last.format.stride << ",\"sizeimage\":" << last.format.sizeimage
            << ",\"driver_buffers\":" << last.format.driver_buffers << ",\"mapped_bytes\":" << last.format.mapped_bytes << ",\"pool_bytes\":" << last.format.pool_bytes
            << ",\"interval_num\":" << last.format.interval_num << ",\"interval_den\":" << last.format.interval_den << "},\n  \"warning\":" << quote(last.warning) << "\n}\n";
    return completed == cycles ? 0 : 130;
  } catch (const std::exception& e) { std::cerr << "CAPTURE_PROBE_FAILED: " << e.what() << std::endl; return 1; }
}
