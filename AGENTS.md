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
pass. No 30min/100-cycle/kernel/ROS2/Agent acceptance. vision_msgs missing.
User requests a plain system-tool dashboard and may apply for jobs tomorrow;
prioritize honest, traceable publication without shrinking the required scope.
