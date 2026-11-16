# M6 · ROS2 data and common control integration

2026-11-16. Functional acceptance passed on the real Orin Nano / USB camera.
The four short publication comparisons are exploratory; the specification's
≥5-minute formal windows and three key repetitions remain pending.
[Evidence and hashes](../evidence/2026-11-16-ros2/manifest.json) preserve original
build/runtime failures, GDB, the corrected source, service/Action events and bag metadata.

## Packages and ownership

`ros2/edgevision_interfaces` contains versioned frame metadata, SetCapture,
GetStatus and RunInspection. `edgevision_ros` is an rclcpp adapter for the existing
core, and `edgevision_bringup` contains launch configuration. Humble vision_msgs
4.1.1 was installed without upgrading JetPack, CUDA, TensorRT or system packages.
The frozen upstream ros_deep_learning remains a reference, not this implementation.

One V4L2Backend owns the camera. PreviewCache uses a preallocated 4MiB owned copy,
5Hz initial throttling and producer try-lock: a slow reader skips previews rather
than retaining FrameLease or waiting in the capture worker. Detection source
images use a separate bounded owned copy. A result/source frame pair must match
generation and internal frame ID before sharing the same ROS stamp.

The GPU worker, control owner and ROS executor are separate threads. Control,
data, diagnostics and Action callbacks have separate groups. Model loading and
inference do not run in a ROS callback. Service, Action and automatic detection
all use the same ControlRouter/TaskManager. Camera stop still waits for the actual
GPU completion and lease drain.

External task records retain idempotency with a fixed 4,096-record budget; reaching
it explicitly rejects admission. Only collected, physically finished **internal
auto-detection** records are explicitly released. Their request IDs are not reusable
by clients. `ros-auto-` and `ros-boot-` prefixes are reserved. The default core
retention remains 256; no silent eviction policy was added.

## Interfaces

| Name | Type | Semantics |
|---|---|---|
| /edgevision/image | sensor_msgs/Image | rgb8 preview, SensorDataQoS/best effort/depth 2 |
| /edgevision/detection_image | sensor_msgs/Image | Exact owned source of the paired detection result |
| /edgevision/detections | vision_msgs/Detection2DArray | COCO class ID string, confidence and pixel box; no invented tracking ID |
| /edgevision/frame_meta | FrameMeta | source, task/frame/generation, driver flags/time domain and monotonic times |
| /diagnostics | DiagnosticArray | Separate capture/model states, errors and bounded health metrics; reliable depth 5 |
| /edgevision/set_capture | SetCapture | Explicit generation/request_id/config ID/deadline; accepted is not success |
| /edgevision/get_status | GetStatus | Actual overall state, optional task query and metrics |
| /edgevision/run_inspection | RunInspection Action | Initial operation=detect; feedback, terminal result, deadline and cancel |

Header frame_id is `camera_optical_frame`. Header stamp uses publication ROS time,
explicitly labeled `publication_ros_time`; it is not a cast of V4L2/steady time.
Driver MONOTONIC/SOE metadata remains separate. Camera calibration is false, so
no fabricated CameraInfo, extrinsics, depth or object distance is published.
COCO IDs/names use the frozen `models/coco80-labels.txt` mapping from M3.

## Real functional results

- Windows core 11/11 and target ASan/UBSan/LSan 13/13 passed. The added tests cover
  independent preview storage and refusal to release logically canceled work
  while physical execution is pending.
- Native ament/colcon built all three packages. Actual topic list/info, RGB data
  dimensions, image/detection source stamps, metadata and diagnostics passed.
- The slow subscriber slept 0.4s/callback and received 24 messages in 10.008s.
  Capture progressed by 330 frames in the observed interval, queue≤2/pool≤4,
  pool exhaustion and sequence gaps were zero. Low-frequency detection deliberately
  causes READY replacement; 273 such drops were recorded, not called driver loss.
- SetCapture real start/first-frame completion, explicit stop/physical drain,
  duplicate request, stale generation, reserved ID and invalid configuration passed.
- Action success returned actual task/generation/frame IDs. Detecting after stop
  returned FAILED/CAPTURE_NOT_RUNNING. Cancel and 50ms deadline passed in the
  separately named 200ms **actual GPU injection** executable. Before cancel the
  matching task was physically busy with a real GPU submission. EXPIRED feedback
  remained execution_pending until cleanup. The product executable has no injection switch.
- A private 4,204,973-byte bag was recorded and replayed; the observer received six
  replayed image messages. Metadata and content hashes are public; the full bag
  and image payloads remain private. Best effort does not imply every recorded
  message must be delivered to a newly discovered subscriber.
- Actual RGB camera output was decoded and visually inspected. This scene is dark
  and blurred, and detections are empty. It proves real publication/association,
  not positive-object accuracy; M3/M5 fixed-image regression supplies model evidence.

## Short-window resource comparison

Each configuration had six seconds of warmup and one approximately 20-second
measurement. All ran the same fused YOLO/camera path at approximately 30 completed
jobs/s. Paired detection-image publication was disabled in this comparison to
avoid introducing a second image stream. Diagnostics-only disables image and
detection publication; 5Hz/30Hz enable image, metadata and detections. The observer
is a separate process and its CPU is retained in the raw record.

| Configuration | Jobs/s | Producer CPU (% of one core) | Peak RSS MiB | Core P95 ms | Core P99 ms |
|---|---:|---:|---:|---:|---:|
| ROS off | 30.04 | 16.64 | 1072.8 | 8.121 | 8.168 |
| Diagnostics only | 29.98 | 16.44 | 1090.7 | 62.299 | 62.466 |
| Image 5Hz | 30.03 | 19.62 | 1111.2 | 42.912 | 43.158 |
| Image 30Hz | 30.02 | 27.48 | 1103.3 | 57.010 | 57.199 |

Core latency is host DQ→completed core detection and excludes DDS delivery.
The ROS adapter admits periodic jobs into the existing FIFO READY queue, while
the direct baseline consumes as frames arrive. The larger frame age must not be
called GPU kernel time or attributed entirely to DDS. One short window cannot
establish precise overhead bounds or prove that 30Hz uses less memory than 5Hz.
Raw clock/temperature conditions and unavailable thermal readings are retained.
The final formal comparison must keep the scheduling policy and latency scope explicit.

## Build, run and deployment identity

```bash
source /opt/ros/humble/setup.bash
export PYTHONNOUSERSITE=1
export CMAKE_BUILD_PARALLEL_LEVEL=2
colcon build --executor sequential --base-paths projects/edgevision/ros2 \
  --build-base projects/edgevision/build/ros2 \
  --install-base projects/edgevision/install/ros2 \
  --cmake-args -DCMAKE_BUILD_TYPE=Release \
  -DEDGEVISION_UPSTREAM_PREFIX="$PWD/build-baseline/aarch64" \
  -DCMAKE_CUDA_COMPILER=/usr/local/cuda/bin/nvcc
source projects/edgevision/install/ros2/setup.bash
ros2 launch edgevision_bringup edgevision.launch.py \
  model:="$PWD/projects/edgevision/artifacts/m3-model-r1/yolov8n.onnx" \
  labels:="$PWD/projects/edgevision/models/coco80-labels.txt"
```

First build failed from incompatible user-site setuptools/packaging; only the
build environment was isolated. A subsequent ament link-signature error was fixed.
The first installed runtime crashed: installation stripped its RUNPATH and loaded
the old `/usr/local/lib/libjetson-inference.so` (SHA555385b4…) instead of the verified
build (SHAa2ea6868…). GDB stopped in that old library's setTensorAddress.
The fix preserves install RUNPATH and verifies actual loaded paths using
dlopen/dlinfo before starting the model worker. An explicit child-only bad
LD_LIBRARY_PATH test now exits 1 with UPSTREAM_RUNTIME_PATH_MISMATCH. No system
library was overwritten and no global loader configuration was changed.

Source R6: 102-file ZIP SHA
`a2ab6669b472b5a1f4b51defc733285427f1d780244fd463439c84a174d4d66e`.
Build/install directory names still contain `ros2-r3`; actual source ZIP and each
supervision log identify the later revision. The board Git HEAD is not the build input.
