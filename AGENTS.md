# EdgeVision development contract

- User's V3.1 specifications are archived in `projects/edgevision/docs/spec/`.
- `projects/edgevision/docs/progress.md` is the current acceptance ledger. Historical
  EdgeVisionRT/MCU milestones do not imply these new milestones passed.
- Freeze upstream 45da40a8f3c180191b269f57f736caaa025b8a69 and its gitlinks.
  Preserve `README.upstream.md`, upstream layout and license.
- First establish an unmodified fixed-image and USB inference baseline. No new
  capture/backend framework before that gate. Record failures without mock fallbacks.
- Personal code goes in `projects/edgevision`; new C++ uses C++17 without changing
  the upstream global standard. Explicitly enable hardware dependencies.
- Record reused/modified/new contributions in `UPSTREAM.md`; commit submodule
  modifications in a personal fork before updating the gitlink.
- Never commit credentials, weights, engines, private camera images or full bags.
  Curated evidence must keep original errors, command arguments and artifact hashes.
- A single owner opens the camera. GPU work retains memory until actual completion.
  Queue capacity, stop wakeups, generation and terminal task states are contracts.
- ROS2, local VLM/Agent, UVC source extension, RTSP and endurance remain required.
  Do not mark them complete based on schemas, source-only builds or old projects.
- Preserve the original two local repositories and their uncommitted work.
- Add meaningful debugging and interview explanations at each verified checkpoint.

## 2026-11-15 checkpoint

M0 evidence collected; unmodified M1 inference SIGSEGV retained. Commit afee249
repairs TRT10 local-context binding, I/O addresses and batch/remaining dimensions.
Final fixed image and 60s USB plus restart pass; full/auxiliary real-GPU probes
pass. No 100-cycle/kernel/ROS2/Agent acceptance. vision_msgs missing.
User requests a plain system-tool dashboard and may apply for jobs tomorrow;
prioritize honest, traceable publication without shrinking the required scope.

M2 core now ports the exact hashed personal J11/J12 storage/queue implementation,
adds FrameMeta, explicit slot/fence states, READY-only generation queue and bounded
TaskManager. Windows 4/4 and Jetson ASan/UBSan/LSan CPU-core 4/4; these tests do
not open a camera or implement a CUDA fence. See docs/m2-core.md for raw API and
record-retention limits. The user intends to power down after the 30min baseline;
finish and retrieve its evidence first, start no new long board test, and leave
normal shutdown to the user unless explicitly requested.

Final 2026-11-15 board checkpoint: repaired upstream 30min completed, 53,898 real
inference/output calls, exit0, camera released, legacy daemon IDLE. Raw 31MB log
and resource CSV are archived separately and hash-verified locally. RSS rose
early and was flat in the last20min; no zero-leak/formal-FPS/multi-hour claim.
User was told normal shutdown is safe; assistant did not shut down the board.
SSH is closed. Recheck connectivity, current power/software/device identity and
camera owners on next entry; do not assume a managed Git HEAD in the board
worktree (actual build input source hashes/archives are the authority there).
