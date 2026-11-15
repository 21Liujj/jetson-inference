# Checkpoint · 2026-11-15 evening

## Completed evidence

- Personal fork default branch: `edgevision/v3.1`; GitHub Pages from `gh-pages`.
- Upstream repair commit: `afee249`; initial docs/evidence: `8ed6584`.
- M2 core commit: `0ddd963`; Windows 4/4 and Jetson CPU sanitizer 4/4.
- Repaired upstream 30-minute baseline: 53,898 inference/output calls, exit 0,
  clean camera release; evidence copied and hash-verified on the Windows host.
- Existing two repositories and their uncommitted work remain untouched.

## Board left at

All task-owned test processes ended, camera unowned, legacy service active but
IDLE. No kernel/module install, JetPack/CUDA upgrade or firmware programming.
The user was advised to perform normal shutdown and remove power afterward.
Assistant SSH sessions closed; physical shutdown/power state was not observed.

## Resume safely

1. Check SSH connectivity and actual board versions, stable camera identity and
   any camera owners. Do not assume the pre-shutdown node number or service state.
2. Use local/public fork code as the development source. Board workspace is
   `/home/nvidia/jetson-inference-edgevision`; its managed Git HEAD is still the
   upstream starting point plus task-owned modifications/transfers. Exact c/
   tensorNet.cpp SHA and core build-input archive link executed code to commits.
   Do not run a blanket reset/pull/clean over its raw evidence and model files.
3. Keep original `/home/nvidia/jetson-inference` and old visual service intact.
   Only one process may stream from the USB camera at a time.
4. Next implementation: port the audited V4L2 I/O seam/session, add eventfd stop,
   negotiated metadata and first-valid-frame RUNNING; connect one ControlRouter,
   new core and upstream inference adapter. Reuse legacy source deliberately.
5. Verify missing/busy camera, no-frame start, slow consumer, generation/reopen,
   100 starts/stops and real disconnect/reconnect before declaring M2 complete.

## Remaining acceptance

Direct capture integration and CUDA event fence are not implemented. No UVC
patch/build/load/rollback; no full reference accuracy suite; no CUDA optimization
timeline; no project ROS2 or real local VLM/Agent integration; no new-mainline
RTSP, 2h or 12h acceptance. `vision_msgs` is missing. Static showcase is not live
hardware control. Never copy the final design's resume bullets as completed work.

User target: implement as much as possible on Nov 16–17, interview Nov 18/19.
Retain all required milestones and report actual completion, not the calendar.
