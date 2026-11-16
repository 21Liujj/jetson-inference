# Upstream & contribution ledger

This is a personal fork of [dusty-nv/jetson-inference](https://github.com/dusty-nv/jetson-inference).
Its NVIDIA MIT license and original layout are preserved. Original project introduction:
[README.upstream.md](README.upstream.md).

| Component | Frozen commit | Role |
|---|---|---|
| jetson-inference | `45da40a8f3c180191b269f57f736caaa025b8a69` | TensorRT inference and applications |
| jetson-utils | `6d5471c97dad443776d2339569a4d1145f034a9e` | Camera/video input, CUDA utilities |
| ros_deep_learning | `9b5a668ba2cb24708d35eb13f734f343be223bd0` | ROS integration reference |
| trt_pose | `6fa5f5d36c9be4bdf314838d840a82dd1f2960d3` | Build dependency |
| camera-capture | `44d4de4a27aeea9dfc23d084b88e498c50a3d199` | Upstream CMake dependency |

Training and container submodules retain their original gitlinks; they are intentionally
not initialized for the minimal inference baseline. `git submodule status` records this.
No submodule source has been intentionally modified in M0/M1.

| Work | Classification | Code | Acceptance |
|---|---|---|---|
| ONNX/TensorRT loading and inference | upstream_reused | `c/tensorNet.cpp`, `c/detectNet.cpp` | M1, see baseline report |
| TRT10 context/I/O initialization and shape repair | upstream_modified | `c/tensorNet.cpp`; commit `afee2497c7fef43eb8b86f1ad845fbe76a8c5dfe` | Original SIGSEGV + GDB retained; fixed image, USB and real-GPU probes pass |
| GStreamer USB camera input | upstream_reused | `utils/camera/gstCamera.cpp` | M1, see baseline report |
| CUDA conversion and normalization | upstream_reused | `utils/cuda`, `c/detectNet.cu` | No personal optimization claimed |
| Environment evidence collector | new | `projects/edgevision/tools/collect_env.py` | Real Jetson command records |
| Baseline runner and analyzer | new | `projects/edgevision/tools` | Real runs linked in baseline report |
| Project documentation and showcase | new | `projects/edgevision/docs`, `projects/edgevision/site` | Link and browser checks |
| Real GPU loader regression | new | `projects/edgevision/tools/trt_compat_probe.cpp`; commit `afee249` | Full and auxiliary-output scenarios execute on target GPU |
| Bounded queue / shared frame storage | new (personal source port) | `projects/edgevision/include/edgevision`, `src/frame`; exact donor hashes in `docs/core-provenance.json` | Windows 4/4; native Jetson sanitizer CPU-core tests 4/4 |
| READY-only frame queue and bounded TaskManager | new | `frame_queue.h`, `task_manager.h`, `src/task_manager.cc` | Generation, idempotency, logical timeout vs physical completion; no real camera/GPU executor claim |
| Typed common ControlRouter | new | `control_router.h`, `src/control_router.cc`, `test_control_router.cc` | Windows core 9/9, Jetson sanitizer suite 11/11; first-frame, stop/cancel, bounded startup/runtime recovery and generation contracts |
| Direct V4L2 and common control integration | new (personal source port + extensions) | `src/camera`, `v4l2_*.h`, `capture_probe.cpp`; donor hashes in `docs/capture-provenance.json` | Real short capture, slow/held consumers, missing/busy device; R2 37-cycle failure preserved, R3 100 cycles with 3 retries; R3 30min 53,999 frames; R4 real unplug recovery, R5 stop/replug passed; R8 direct-input/upstream DetectNet serial adapter verified; R10 shared task execution, real GPU cancellation/expiry/shutdown and joint USB recovery verified |
| Direct-input upstream DetectNet adapter | new adapter / upstream CUDA and detection reused | `upstream_detector.h`, `src/inference`, `capture_inference_probe.cpp` | R8 real 60s/1,802 inferences, same M1 engine; independent missing-model error, actual CUDA completion before lease return. No accuracy/async/performance claim |
| YUYV row layout and independent model state | new | `image_layout.h`, `ControlRouter::observe_model` | Windows 8/8, target sanitizer 10/10; padding/guard/truncation and owner/state separation |
| Common bounded execution backend | new | `execution_backend.h`, `upstream_executor.h`, `src/inference/upstream_executor.cc` | One shared TaskManager; R10 real in-flight deadline/cancel/stop/shutdown and model-failure tests; actual USB failure during pending GPU event, new-generation detection resumes |
| Static YOLOv8n adapter and regression | new adapter / upstream tensorNet reused | `upstream_yolo.cc`, `yolo_preprocess.cc`, `yolo_postprocess.h`, reference/probes; existing personal model artifact audited separately | M3 200-image same-input AP comparison, 24 byte-exact preprocessing cases, real video and shared-task retests; source snapshots and original R4 test failure retained in `evidence/2026-11-16-yolo` |
| Explicit GPU lifecycle acceptance injection | new test utility | `tools/gpu_acceptance_delay.cu`, executor probes | Disabled by default; real GPU outstanding-work observation, no CUDA optimization/performance claim |
| Bounded HealthMonitor | new | `health_monitor.h`, `src/health_monitor.cc` | Window capacity/time, unknown values, generation isolation, CPU latency samples; fixture and real capture telemetry, no GPU benchmark |
| Recovery acceptance probe | new | `tools/capture_recovery_probe.cpp` | Actual fault-to-first-frame and alive-after-stop observation; invalid R4 operator window retained, see `docs/m2-runtime.md` |
| Agent repository routing and domain skills | new | `AGENTS.md`, `agent.md`, `.agents/skills` | Eight scoped development skills; not a completed runtime VLM/Agent |
| Matched unchanged UVC build / runtime rollback | upstream_rebuilt; new verification tools | `tools/fetch_kernel_sources.py`, `build_uvc_module.py`, `verify_uvc_baseline.py` | M3 kernel gate: original srcversion/vermagic, 155 import CRCs; actual original/rebuilt/original captures |
| UVC frame and stream trace extension | upstream_modified (GPL driver patch) / new parser | `kernel/0001-uvc-frame-observability.patch`, trace tools | M4 two matrices, 172 import CRCs, actual private internal-recycle observations, 1,803-row real USB recovery correlation, loaded identity and actual stock rollback; see `docs/m4-uvc-trace.md` |
| Fused RGB preprocessing and bounded event pipeline | new kernel/adapter; upstream tensorNet reused | `cuda_letterbox.cu`, `gpu_yolo_pipeline.cc`, pipeline probes/matrix/analyzer | M5: 48 preprocessing cases, same 200-image AP gates, 15 unprofiled runs, actual cross-stream GPU intervals, real camera/shared-task cancellation and physical cleanup; runtime P1 versus offline P2 limits in `docs/m5-gpu-pipeline.md` |
| ROS2 common runtime adapter | new / frozen ros_deep_learning reference retained | `projects/edgevision/ros2`, PreviewCache, explicit internal-record release, real acceptance tools | M6 real topics/services/Action GPU cancellation/deadline, slow subscriber, private bag/replay; build/install loader failure retained. Short four-group measurements are exploratory; formal windows pending |
| Local model-driven Agent | in progress | Audited personal EdgeQwen artifacts and owned adapter | Independent Qwen3-VL VQA passed; complete constrained planning/shared tools/20-task acceptance remains M7 |

Personal commits are the commits after the frozen parent on `edgevision/v3.1`:
`git log 45da40a8..edgevision/v3.1 -- projects/edgevision UPSTREAM.md README.md`.
For each later source change, append its actual commit and evidence here.
