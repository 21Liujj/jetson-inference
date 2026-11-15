# V3.1 acceptance ledger

Last updated: 2026-11-15, Asia/Shanghai. This ledger applies to the new
jetson-inference fork. Old M/J/F milestones in the two legacy repositories have
different definitions and are not carried over as completed stages.

| Stage | Current evidence state | Actual result / remaining gate |
|---|---|---|
| M0 environment and audit | board_verified | Locked upstream/gitlinks, real hardware and dependencies collected; `vision_msgs` is missing |
| M1-A unmodified upstream | board_verified (build/camera); blocked (inference) | Build succeeds after missing dev dependency installation; original TensorRT 10 loader crashes; raw failure retained |
| M1-B compatibility repair | board_verified, short runs only | Fixed image, 60s USB inference, restart, negative tensor-name test, two real GPU probes pass |
| M1 longer repaired baseline | board_verified | 1800.6016s process lifetime, 53,898 inference/output calls, normal shutdown; M1-A failure remains a failure |
| M2 capture/task framework | host_verified / board_verified (core only) | FrameMeta, bounded queues, pool states/fences and TaskManager; Windows 4/4 and Jetson ASan/UBSan/LSan 4/4; real capture/router integration remains |
| M3 model and kernel preparation | planned | 20-image regression, frozen annotated set, native module baseline |
| M4 UVC extension | planned | Source patch, original/disabled/enabled trace comparisons, install and actual rollback |
| M5 CUDA and async optimization | planned | Numerical regression, two memory paths, event pipeline, Nsight timeline |
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
- 30-minute repaired baseline: [summary and raw evidence](../evidence/2026-11-15-30min/summary.json).
- Failure/root-cause record: [debugging_journal.md](debugging_journal.md).

## Next three engineering steps

1. Port the audited V4L2 I/O seam and session; add eventfd stop, true negotiated
   metadata, first-valid-frame RUNNING and exact drop accounting.
2. Connect the new core through one ControlRouter and upstream inference adapter;
   validate actual camera ownership, slow consumers, failures and restart.
3. Complete 100 start/stop cycles and direct-backend long capture, then begin
   M3 fixed-image numerical regression and matching kernel source preparation.

First interview checkpoint is the real TensorRT 10 loader failure and repair.
It does not assert that the learner has already mastered the remaining stages.
