# V3.1 acceptance ledger

Last updated: 2026-11-16, Asia/Shanghai (development resumed). This ledger applies to the new
jetson-inference fork. Old M/J/F milestones in the two legacy repositories have
different definitions and are not carried over as completed stages.

| Stage | Current evidence state | Actual result / remaining gate |
|---|---|---|
| M0 environment and audit | board_verified | Locked upstream/gitlinks, real hardware and dependencies collected; `vision_msgs` is missing |
| M1-A unmodified upstream | board_verified (build/camera); blocked (inference) | Build succeeds after missing dev dependency installation; original TensorRT 10 loader crashes; raw failure retained |
| M1-B compatibility repair | board_verified, short runs only | Fixed image, 60s USB inference, restart, negative tensor-name test, two real GPU probes pass |
| M1 longer repaired baseline | board_verified | 1800.6016s process lifetime, 53,898 inference/output calls, normal shutdown; M1-A failure remains a failure |
| M2 capture/task framework | board_verified (baseline gates complete) | Windows 9/9; Jetson sanitizer 11/11. R3 capture 100 cycles/30min, R8 direct-input 60s, R10 shared task API, real GPU timeout/cancel/stop/shutdown, model failure and joint USB recovery all verified. Component source versions remain separate; no final-stack endurance claim |
| M3 model and kernel preparation | board_verified | Matched unchanged UVC build, 155 import CRCs and actual stock rollback pass. 200-image FP16 AP drop 0.047 percentage point, fixed-box match 99.5%; 24 CPU letterbox cases byte-exact; 60s YOLO video, YOLO/SSD seven-case task retests, Windows10/10 and Jetson sanitizer12/12 pass; source and original failures archived |
| M4 UVC extension | board_verified | GPL frame/stream patch, 172 import CRCs, stock/disabled/enabled normal/slow/start-stop matrices, raw traces and parser; real unplug 7.3799s recovery, 1,803 consumed records correlated; actual stock rollback after all phases. Observational overhead only; no driver bug-fix or precise overhead-bound claim |
| M5 CUDA and async optimization | in progress | P0 serial Nsight diagnostic collected; fused preprocess, memory-path comparison, fixed buffers/event pipeline and formal before/after gates remain |
| M6 ROS2 | planned | Topics, diagnostics, service, Action, QoS/cancellation tests, real bag |
| M7 VLM/Agent | planned | Local model independently verified; schema/tools, 20 tasks and 3 real traces |
| M8 system delivery | planned | Live Web/RTSP, bounded logs, deployment and failure recovery; 30min/2h/12h endurance |

The static showcase is documentation, not the M8 live device console. Existing
ROS installation is not project integration. The regression probe uses real GPU
execution but is not a model-accuracy or optimization benchmark.

## Verified checkpoint

- Code repair: `afee2497c7fef43eb8b86f1ad845fbe76a8c5dfe`.
- Evidence: [manifest](../evidence/2026-11-15/manifest.json),
  [complete text logs](../evidence/2026-11-15/raw-text-evidence.zip).
- Commands, results and restrictions: [baseline.md](baseline.md).
- Existing work preserved: [repo-audit.md](repo-audit.md).
- M2 core implementation and limits: [m2-core.md](m2-core.md).
- M2 real capture and original failure/retest: [m2-capture.md](m2-capture.md).
- Shared execution lifecycle and M2 acceptance: [m2-executor.md](m2-executor.md).
- Matched-source UVC build and actual stock rollback: [m3-kernel.md](m3-kernel.md).
- YOLO model and fixed-input accuracy baseline: [m3-model.md](m3-model.md).
- UVC extension and real frame correlation: [m4-uvc-trace.md](m4-uvc-trace.md).
- Serial upstream GPU input adapter: [m2-inference.md](m2-inference.md).
- Runtime recovery/health and R3 capture endurance: [m2-runtime.md](m2-runtime.md).
- 30-minute repaired baseline: [summary and raw evidence](../evidence/2026-11-15-30min/summary.json).
- Failure/root-cause record: [debugging_journal.md](debugging_journal.md).

## Next three engineering steps

1. Audit the P0 serial GPU timeline, freeze M5 numerical gates and implement a
   fused RGB letterbox/layout/normalization path with bounded reusable buffers.
2. Measure pinned/device versus mapped inputs and P0/P1/P2 event-dependent
   execution on identical inputs; retain raw Nsight reports and latency scopes.
3. Integrate ROS2 and the real local VLM/Agent, then live delivery/endurance.
   vision_msgs remains missing; cached Qwen3-VL model is not yet project-verified.

2026-11-16: user explicitly resumed development after powering the board back on.
The prior stop request is revoked. Recheck hardware/source identity, then close
the remaining M2 gates; preserve all later milestone requirements.

First interview checkpoint is the real TensorRT 10 loader failure and repair.
It does not assert that the learner has already mastered the remaining stages.
