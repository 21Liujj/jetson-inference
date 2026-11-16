# M5 · CUDA preprocessing and event-dependent execution

2026-11-16, Orin Nano 8GB / CUDA12.6 / TensorRT10.3. This stage extends the
repaired upstream `tensorNet`; it does not introduce a separate TensorRT loader.
[Curated evidence](../evidence/2026-11-16-cuda/manifest.json) retains the actual
source inputs, raw Nsight reports, run logs, timings and numerical reports.

## Input contract and correctness

The new kernel accepts RGB8 with an explicit row stride. Upstream still performs
camera YUYV→RGB conversion; raw YUYV/NV12 support is not attributed to the new kernel.
It fuses half-pixel bilinear letterbox, 114 padding, RGB HWC→FP32 CHW and /255.
The 256-thread block writes consecutive positions within each output plane.
Three channel planes are written once, removing CPU letterbox and the previous
device→host RGB / host→device FP32 round trip in the camera path.

Thresholds were recorded in `models/cuda-preprocess-gates.json` before execution.
24 images × pinned/device and mapped inputs = 48 GPU preprocessing cases passed,
including odd sizes, border pixels, 17-byte row padding and rejected truncation.
Max absolute error was 0.003921598196; maximum per-image mean absolute error was
0.0004806570448. This is bounded one-byte interpolation variation, not byte equality.

The same 200 COCO images plus four synthetic inputs used in M3 were rerun:
AP(reference)=0.3948222949543204, AP(fused)=0.39417026373627834,
drop=0.0652031 percentage point (gate≤0.5). Fixed high-confidence matching was
199/200=99.5% (gate≥98%); the independent C++/NumPy output parser had no failures.
Pinned, mapped and P2-pinned produced identical 24 raw output tensors and all 204
detection records. The original threshold-crossing case remains in the report.

## Ownership and actual completion

`GpuYoloPipeline` allocates 1–3 fixed slots before processing, with a 4MiB RGB
budget, device input tensor and mapped FP32 output per slot. It owns two
nonblocking streams and persistent events. Pinned input uses `cudaHostAlloc` +
`cudaMemcpyAsync`; mapped input uses `cudaHostAllocMapped` + a device alias.
Both paths use the same mapped output, so this comparison isolates input storage.

The producer event precedes preprocessing; the inference stream waits for the
preprocessing event. One upstream execution context is retained. Its previous
actual CUDA completion event must be ready before rebinding its I/O addresses.
An output slot becomes reusable only after physical completion and the CPU output
copy. Logical cancellation invalidates output and leaves storage owned until then.
Destruction drains prepared work and in-flight inference before freeing I/O.
An unrecoverable unknown completion exits at the worker/process boundary.

The shared task executor uses the **one-slot P1 fused path**. P2 is an independently
validated two-slot component/offline benchmark; continuous-camera P2 throughput
is not claimed. The default SSD and explicit CPU YOLO baseline remain available.
Real camera GPU deadline, cancellation, priority-stop, restart and shutdown tests
passed all seven cases; final frame-pool use and video mappings were zero.

## Same-input short-window unprofiled comparison

Each variant used the same engine SHA `2925e839…0474519`, preloaded fixed RGB inputs
and display NMS. Three rotated-order repetitions, 60 excluded warmups and 1,000
measured completions per repetition. Values below are medians of the three run
statistics, not pooled percentiles. These runs are shorter than the specification's
≥5-minute formal windows; that final performance gate remains pending. Latency runs from host staging to actual GPU
completion and common CPU NMS, including pipeline scheduling. File writes and model
initialization are excluded from per-frame latency. Throughput includes the bounded
CSV timing recorder; dataset image/tensor writes are disabled in speed runs.
These are offline images/s, not USB camera FPS.

| Variant | Images/s | P95 ms | P99 ms |
|---|---:|---:|---:|
| P0 CPU letterbox, serial upstream inference | 73.76 | 14.445 | 14.852 |
| P1 fixed pinned/device slot | 129.99 | 11.366 | 11.491 |
| P1 fixed mapped input slot | 130.08 | 11.355 | 11.500 |
| P2 two pinned/device slots | 190.17 | 14.380 | 14.566 |
| P2 two mapped input slots | 194.12 | 14.020 | 14.216 |

P1-pinned improved throughput by about 76% and P99 by about 23% relative to P0.
P2-pinned improved throughput further but increased P99 about 27% relative to P1:
queuing and CPU postprocessing remain relevant. The close mapped/pinned results
do not establish a general mapped-memory advantage on Jetson.

CPU min/max/current stayed at 1.728GHz and GPU at 1.020GHz in every recorded matrix
sample. The existing MAXN_SUPER/EMC override state was read, not changed. Thermal
samples and unavailable inactive thermal zones are retained. The raw P0 event
columns contain zero placeholders because that probe did not instrument GPU stage
times; those values must be read as **unmeasured**, not zero-duration GPU work.

## Nsight and the measured hotspot

The serial real-camera diagnostic recorded approximately three `cudaMemcpy` and
two `cudaDeviceSynchronize` calls per inference. Initialization is included in
that diagnostic and must not be treated as steady-state time.

P2 NVTX marks host staging, async copy, fused preprocessing, upstream enqueue,
completed-output collection and common NMS. The actual GPU interval analysis
excluded 60 warmup fused kernels: 597/600 subsequent fused kernels intersected a
TensorRT kernel on another stream, totaling 4,627,712ns / 53,415,808ns = **8.66% of
fused kernel time**. This is not 8.66% end-to-end speedup. Original stream IDs,
kernel names and interval examples are retained, together with the `.nsys-rep`.

Profiling reduced this P2 run to 142.74 images/s versus the unprofiled median
190.17; use the unprofiled matrix for performance. Nsight Systems establishes
timeline/API behavior, not hardware cache-hit or achieved-bandwidth counters.
The coalescing explanation comes from the kernel's access pattern, not an invented
Nsight Compute measurement. No extra inference context or CUDA Graph is claimed.

The device reports mapped/host-register/managed support, two async engines and
`concurrentManagedAccess=0`. Managed input and arbitrary driver-MMAP registration
are not used. DMA-BUF import remains conditional on driver/import compatibility;
neither is claimed as verified by this stage.

## Five-minute windows and three repetitions

The later duration-controlled comparison ran from 20:01:19 to 21:17:07 local time.
Five configurations each completed three unprofiled windows of at least 300s,
excluding 60 warmup jobs per run. All **646,305** measured CSV rows were checked:
unique contiguous ordinals, start/end/latency consistency, finite stage timings,
completion count, throughput and exact nearest-rank P50/P95/P99. This does not
re-run the numerical output regression; the detector/kernel implementation is
unchanged from the separately verified numerical source.

| Configuration | Median images/s | P99 ms, each of three runs | Total samples |
|---|---:|---|---:|
| P0 CPU | 74.086 | 14.767 / 14.751 / 14.709 | 66,593 |
| P1 pinned/device | 129.716 | 11.541 / 11.607 / 11.549 | 116,754 |
| P1 mapped | 129.966 | 11.476 / 11.612 / 11.519 | 116,787 |
| P2 pinned/device | 192.682 | 14.445 / 14.377 / 14.228 | 173,511 |
| P2 mapped | 192.115 | 14.413 / 14.433 / 14.430 | 172,660 |

P1 pinned improves median throughput about 75.1% and median per-run P99 about
21.7% against P0. P2 pinned improves throughput a further 48.5% against P1 while
its median per-run P99 increases about 24.5%. The raw distributions and minute
start-time buckets are retained; a median of run quantiles is descriptive and
does not replace a pooled distribution or a long-window P99.

The 9,024 telemetry samples have a median interval of 0.504s. All recorded CPU
min/max/current frequencies were 1.728GHz and GPU frequencies 1.020GHz. Highest
readable thermal sample was 68.843°C; three inactive zones remained unavailable.
System MemAvailable was 4,970,700–6,011,680KiB. This is system availability, not
process RSS or GPU allocation size. **Per-process RSS, CPU/GPU utilization and
power rails were not collected by this frozen sampler; the required resource
comparison remains pending.** loadavg cannot substitute for CPU utilization.

Latency ends before the per-row CSV write. Throughput includes the bounded CSV
recorder and final physical drain; image/tensor dumping is disabled. The original
summary's file-write phrase must be read with its explicit CSV-in-throughput flag.
P0 GPU event zeros remain uninstrumented placeholders. No 190fps camera claim,
ROS performance, Agent interference or final endurance result follows from this test.

[Formal evidence and hashes](../evidence/2026-11-16-gpu-formal/manifest.json)
include all 15 runs, 60s latency buckets, original conditions and run logs, the
actual 89-file source ZIP, original supervised output and analysis script. Source
ZIP SHA256: `5d846473fa160c4515549b7e6dc667e375a48ec0943f3c7981aa5b42aa8d78ec`;
probe SHA256: `d5a9da9677da5461c29b4d930344fc6bfcfaf71f4933e8bcdc990b77da85bc42`.
Only the benchmark duration controls changed from the prior functional source.

## Camera verification and reproduction

The fused real USB run completed and published 1,802 inferences in its 60-second
window, with zero sequence gaps or pool exhaustion. There were 57 READY replacements
across the whole process including initialization; this is retained, not mislabeled
as kernel loss. All detections were empty in that scene. Accuracy comes from the
fixed dataset, not these camera images. Final pool use was zero; stop took 11.88ms.

```bash
cmake -S projects/edgevision -B projects/edgevision/build/inference-native \
  -DEDGEVISION_ENABLE_V4L2=ON -DEDGEVISION_ENABLE_UPSTREAM_INFERENCE=ON \
  -DEDGEVISION_ENABLE_CUDA_PIPELINE=ON \
  -DEDGEVISION_UPSTREAM_PREFIX="$PWD/build-baseline/aarch64" \
  -DCMAKE_CUDA_COMPILER=/usr/local/cuda/bin/nvcc
cmake --build projects/edgevision/build/inference-native -j2
# run_gpu_matrix.py requires an explicit model, engine, corpus and NEW out path.
# analyze_gpu_profile.py accepts the SQLite export of the retained Nsight report.
```

M5 build input: 84-file source ZIP SHA256
`9ed1bbb4c5814927ebfa676c0b8fe0feeff53a1b0911db56deba67e69f8cf6d0`.
Later ROS preview/retention additions have their own source snapshot; they do not
retroactively change these M5 measurements. Final-stack endurance remains M8.
