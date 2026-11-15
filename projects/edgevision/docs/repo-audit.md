# M0 · Repository audit

Audit date: 2026-11-15 (Asia/Shanghai). Scope: user's V3.1 Jetson system.

## Authority and protected work

The two supplied implementation prompts have the same SHA256:
`c21a0f431e4022a6546e10683a517690ae2de23d88de22fee9e9501714900605`.
The Markdown construction plan is archived under `spec/`; its PDF is a 24-page
formatted rendering of V3.1. PDF page 5 visually confirms the baseline-first and
fork/submodule requirements. Markdown is used for exact requirements because PDF
Chinese text extraction has broken glyph mappings.

| Existing repository | HEAD | Audit finding | Treatment |
|---|---|---|---|
| `21Liujj/EdgeVisionRT` | `666a3a1d33d4637e267dd858c9af91ff83827d57` | Clean local tree; standalone C++ daemon, TensorRT and Python cascade | Retain as historical source/donor |
| `21Liujj/jetson-stm32F407` | Same tracked HEAD | 15 tracked modified files plus substantial untracked MCU/J09–J12 work | Preserve all changes; no reset/stash/blanket commit |
| Board `jetson-inference` | `c038530ebf718e6867c4458c3e439406020732ff` | Old upstream clone, two untracked Caffe model files | Preserve; separate worktree at required commit |
| New personal fork | `45da40a8f3c180191b269f57f736caaa025b8a69` parent | Upstream layout and locked submodules | New mainline for the V3.1 project |

The legacy projects already contain camera, frame leases, queue improvements,
YOLO inference, RTSP and VLM wrappers. These are implementation candidates, not
proof of V3.1 acceptance. J12 reports shared frame-pool lifetime fixes and actual
ASan reproduction; review those contracts before porting. Do not revert to the
older raw-pool lease in the clean EdgeVisionRT snapshot.

## Current board findings

- Real SSH connection to the user's Jetson; aarch64, Linux `5.15.148-tegra`.
- Ubuntu `22.04.5`, L4T `36.4.3`, CUDA compiler `12.6.68`.
- TensorRT packages `10.3.0.30-1+cuda12.5`; ROS2 Humble installed.
- `nvcc`, `trtexec`, `ros2` were absent from the initial shell PATH; binaries / ROS
  setup exist. PATH absence is distinct from missing software.
- USB2 high-speed UVC camera, capture node index0 and metadata node index1.
  Stable `/dev/v4l/by-id` identifier is recorded in raw environment evidence.
- YUYV: 640×360/800×480/960×544 at 30fps; 1280×720 at 15fps; 1920×1080 at 10fps.
  MJPG supports the enumerated sizes at 30fps. **640×480 is not advertised.**
- Existing `edgevision.service` was active but daemon state `IDLE`, with no camera
  owner reported by `fuser`. It was not stopped, reconfigured or replaced.
- Original UVC module is a module under `/lib/modules/5.15.148-tegra/`; no kernel
  or module installation has been performed in M0/M1.

## Reproducibility details

The board worktree is `/home/nvidia/jetson-inference-edgevision`.
The original clone stays `/home/nvidia/jetson-inference`.
The minimal build initializes utils, pose, camera-capture and ROS at original
gitlinks. Network clone failed with GnuTLS -110; local object sources at the same
SHA were used through command-scoped submodule URL overrides, without changing
tracked `.gitmodules` or advancing any submodule.

Windows NTFS checkout reports case-collision dirt in upstream generated HTML and
`python/python/Jetson/Inference/__init__.py`, plus the analogous utils path.
These are fresh-checkout artifacts, excluded from personal staging. Linux is the
authoritative hardware build. No such generated files are part of our contribution.

The stock CMake prebuild script runs apt and pip upgrades and offers PyTorch.
It is intentionally disabled with `BUILD_DEPS=OFF` and `BUILD_INTERACTIVE=OFF`.
Only missing development dependencies required by observed compiler errors are
installed, after reviewing apt's proposed changes. See debugging journal.

## First personal changes after M1

1. Reuse `libjetson-inference`, `libjetson-utils`, existing numerical operators and
   the GStreamer camera as the reference backend.
2. Audit and port the legacy move-only frame lease and bounded queue into the
   personal system layer; preserve J11/J12 ownership fixes and negative tests.
3. Add FrameMeta, eventfd-stop V4L2 backend, TaskManager and ControlRouter with
   first-valid-frame RUNNING semantics and generation-aware terminal results.

M2 source changes wait for a real M1 fixed-image plus USB inference gate.
