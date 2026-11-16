#include "edgevision/health_monitor.h"
#include <cassert>
#include <cstdio>
#include <cmath>
#include <future>
using namespace edgevision;
static CaptureHealthSample sample(uint64_t generation, uint64_t at, uint64_t frames) {
  CaptureHealthSample s; s.generation = generation; s.observed_mono_ns = at;
  s.counters.valid_frames = frames; s.counters.queue_capacity = 2; s.counters.pool_capacity = 4; return s;
}
int main() {
  HealthMonitor::Limits limits; limits.capture_samples = 4; limits.latency_samples = 3;
  HealthMonitor health(limits); RouterStatus router; router.capture = CaptureState::Running; router.generation = 1;
  auto s = health.snapshot(1, router);
  assert(!s.has_capture_sample && !s.rate.valid_capture_fps && !s.dequeue_to_result.p99_ns && !s.process);
  assert(health.observe_capture(sample(1, 1000000000, 10)));
  assert(!health.snapshot(1000000001, router).rate.valid_capture_fps);
  assert(health.observe_capture(sample(1, 2000000000, 40)));
  s = health.snapshot(2000000000, router);
  assert(s.rate.samples == 2 && s.rate.interval_ns == 1000000000 && std::abs(*s.rate.valid_capture_fps - 30.0) < 1e-9);
  FrameMeta last;
  unsigned i = 0;
  for (uint64_t duration : {10, 20, 30, 40, 100}) {
    FrameMeta m; m.session_generation = 1; m.frame_id = i;
    m.host_dequeue_mono_ns = 3000000000ULL + uint64_t(i++) * 10000;
    m.publish_mono_ns = m.host_dequeue_mono_ns + 1;
    assert(health.observe_consumed(m, m.host_dequeue_mono_ns + duration)); last = m;
  }
  s = health.snapshot(3000050000ULL, router);
  assert(s.dequeue_to_consume.samples == 3 && s.dequeue_to_consume.p50_ns == 40 && s.dequeue_to_consume.p95_ns == 100);
  assert(s.dequeue_to_consume.p99_ns == 100 && s.frame_age_from_dequeue_ns == 10000 && s.last_frame_id == 4);
  assert(!s.dequeue_to_result.p99_ns); // no fabricated inference metric
  assert(health.observe_result(last, last.host_dequeue_mono_ns + 1000));
  assert(health.snapshot(3000050000ULL, router).dequeue_to_result.p50_ns == 1000);
  assert(!health.observe_consumed(last, last.host_dequeue_mono_ns));
  assert(!health.observe_capture(sample(1, 2100000000, 39))); // counter regression, same generation
  assert(health.observe_capture(sample(1, 5100000000ULL, 40)));
  s = health.snapshot(5100000000ULL, router);
  assert(!s.telemetry_stale && s.observed_capture_stalled); // reporter alive, frame progress absent
  router.capture = CaptureState::Recovering;
  assert(!health.snapshot(5100000001ULL, router).observed_capture_stalled);
  router.generation = 2;
  s = health.snapshot(5100000001ULL, router);
  assert(!s.generation_matches && !s.rate.valid_capture_fps && !s.frame_age_from_dequeue_ns);
  assert(health.observe_capture(sample(2, 5200000000ULL, 0)));
  s = health.snapshot(5200000000ULL, router);
  assert(s.generation_matches && !s.dequeue_to_consume.p99_ns && !s.last_frame_id && !s.rate.valid_capture_fps);
  assert(!health.observe_result(last, 5200000001ULL));
  assert(health.snapshot(5200000002ULL, router).stale_frame_reports == 1);
  for (unsigned n = 1; n <= 10; ++n) assert(health.observe_capture(sample(2, 5200000000ULL + uint64_t(n) * 100000000, n * 3)));
  s = health.snapshot(6200000000ULL, router);
  assert(s.rate.samples == 4 && std::abs(*s.rate.valid_capture_fps - 30.0) < 1e-9);
  s = health.snapshot(20000000000ULL, router);
  assert(!s.rate.valid_capture_fps && !s.dequeue_to_consume.p99_ns && s.telemetry_stale);
  ProcessHealthSample process; process.observed_mono_ns = 6000000000ULL; process.rss_kib = 1234;
  assert(health.observe_process(process)); s = health.snapshot(6200000000ULL, router);
  assert(s.process->rss_kib == 1234 && !s.process->fd_count);
  assert(!health.observe_process(process));
  assert(!health.observe_capture(sample(1, 6300000000ULL, 99)));
  auto bad = sample(2, 6300000000ULL, 30); bad.counters.queue_depth = 3;
  assert(!health.observe_capture(bad));
  bool threw = false; try { auto l = limits; l.latency_samples = 0; HealthMonitor invalid(l); } catch (const std::invalid_argument&) { threw = true; }
  assert(threw);
  // Producer snapshots must expose a coherent set of counters to another thread.
  HealthMonitor concurrent;
  auto producer = std::async(std::launch::async, [&] {
    for (uint64_t n = 1; n <= 2000; ++n) {
      auto v = sample(1, n, n); v.counters.invalid_payloads = n * 2;
      assert(concurrent.observe_capture(v));
    }
  });
  for (unsigned n = 0; n < 1000; ++n) {
    const auto v = concurrent.snapshot(3000, router);
    assert(v.counters.invalid_payloads == v.counters.valid_frames * 2);
  }
  producer.get();
  std::puts("PASS bounded health windows, unknown values, rate/quantiles, generation, stale reports, progress and coherent snapshots; CPU only");
}
