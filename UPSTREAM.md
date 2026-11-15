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
| Direct V4L2 and common control integration | planned | `projects/edgevision` | M2 core has passed; real capture/router integration remains |
| UVC frame trace extension | planned | Target-L4T kernel patch | M3/M4; no modified module claimed |
| CUDA event pipeline | planned | Execution layer | M5; numerical regression + timeline required |
| ROS2 + model-driven Agent | planned | ROS workspace + Agent adapter | M6/M7; real model/tool traces required |

Personal commits are the commits after the frozen parent on `edgevision/v3.1`:
`git log 45da40a8..edgevision/v3.1 -- projects/edgevision UPSTREAM.md README.md`.
For each later source change, append its actual commit and evidence here.
