# M2 · Core ownership and task contracts

This is the first part of M2, not a completed capture or ROS/Agent service.
The J11/J12 personal bounded queue and shared frame-pool storage are ported from
the protected legacy worktree. [Exact donor hashes](core-provenance.json) distinguish
uncommitted donor changes from the older published Git HEAD.

## Implemented

- `FrameMeta`: driver sequence, capture generation, negotiated geometry, fourcc,
  stride, bytesused, flags and driver timestamp domain; host dequeue/publish use
  separate monotonic nanoseconds. Slot epoch is distinct from capture generation.
- Shared pool storage survives pool-handle destruction. Move-only leases retain
  storage and only return the matching epoch. Pool construction validates its
  256 MiB/1024-slot ceiling before allocating storage.
- Explicit FREE / WRITING / READY / INFLIGHT / DONE states. An in-flight lease
  owns a completion fence and waits for physical completion before releasing its
  slot. There is no real CUDA fence implementation in this phase.
- READY-only `FrameQueue`, default capacity 2; discard the oldest pending frame
  for freshness, never an in-flight frame. Queue generation mismatch is explicit.
- Generic queue keeps nonblocking admission, drain/abort, stop wakeups and release
  outside its mutex. It can reject newest work or replace oldest waiting work.
- TaskManager: default one waiting task / one active executor, bounded retained
  records, request-id deduplication and conflicting-parameter rejection; deadline,
  cancellation and stale-generation terminal results.
- Logical task termination is separate from physical execution completion.
  EXPIRED/CANCELED/STALE work keeps the executor occupied until backend completion;
  a late successful backend result cannot overwrite that terminal result.
- Typed `ControlRouter` validates bounded capture settings, canonicalizes
  parameters and routes start/stop through the same TaskManager. Transport
  threads submit/query/cancel; one owner polls and changes capture state.
- STARTING becomes RUNNING only after STREAMON plus validated first-frame
  metadata. Capture and model states remain separate. A start advances the
  execution generation without changing the original idempotency request.
- Stop during a no-frame STARTING cancels that start and requests cleanup
  immediately. Stop/cancel/expiry retain physical execution until a backend
  Stopped acknowledgement. A partial-start exception also requires cleanup.

The completion interface deliberately does not declare GPU completion based on
a timer. The CPU latch/fence fixtures test ownership contracts only. A CUDA event
adapter and real cancellation/timeout GPU tests belong to M5.

## Build and verification

Core-only CMake never probes CUDA, TensorRT, GStreamer, ROS2 or a camera:

```bash
cmake -S projects/edgevision -B projects/edgevision/build/host -DCMAKE_BUILD_TYPE=Debug
cmake --build projects/edgevision/build/host -j2
ctest --test-dir projects/edgevision/build/host --output-on-failure

# Linux GCC/Clang ownership regression; LeakSanitizer enabled explicitly.
cmake -S projects/edgevision -B projects/edgevision/build/asan \
  -DCMAKE_BUILD_TYPE=Debug -DEDGEVISION_ENABLE_SANITIZERS=ON
cmake --build projects/edgevision/build/asan -j2
ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 \
  ctest --test-dir projects/edgevision/build/asan --output-on-failure
```

Windows uses GCC 15.2.0/UCRT64 with CMake and Ninja selected explicitly; only the
build process PATH is changed. Tests retain assertions even for a release build.
Current result: Windows **5/5**, including the new router contract;
native Jetson with ASan/UBSan and LeakSanitizer enabled **4/4**, for the earlier
core revision. [Core evidence](../evidence/2026-11-15-m2-core/) and
[new Windows router evidence](../evidence/2026-11-15-m2-router/).
The new router and TaskManager generation changes have not run on the Jetson.
The native Jetson tests are CPU core tests: they do not open the camera or execute
a GPU model. There is no silent mock capture backend.

Tests cover 20,000 concurrent admissions, multiple blocked queue consumers,
resource destruction that reenters the queue, 1,000 old pool generations,
completion-fenced release, READY-only freshness/generation checks, 16 concurrent
duplicate requests, deadlines, queue budgets, cancellation and late results.
Router tests add first-frame gating, no-frame stop, cleanup acknowledgement,
canceling an active stop, stale events, partial-start/poll exceptions, backend
status reentry without a held router lock and rejection of a second control owner.

## Explicit limits and next integration

- Real V4L2 eventfd stop, device first-frame validation, reconnect, 100
  start/stop cycles and the upstream videoSource/DetectNet adapter are not yet
  ported. Enabling the V4L2 CMake option currently fails with an explicit message.
- FramePool raw index APIs are retained from the donor. Only the producer may use
  `slot_ref` before handoff; fabricating multiple leases for one slot or accessing
  a handed-off raw slot violates the owner contract. A fenced in-flight slot
  cannot be freed through `release(index)`.
- Only READY frames enter FrameQueue. Completion-fence waiting can block its
  owning executor; capture and control threads must never own an in-flight lease
  whose destruction would wait for a GPU event.
- Router currently accepts typed start/stop only. Other commands return
  UnsupportedCommand; JSON/ROS/Agent transports and named configuration IDs
  are not connected. TaskManager does not execute text.
- The backend is a mandatory non-owning interface with no production mock.
  Real hardware calls belong to its worker, outside the router mutex. The
  application must drain stop before destroying the router; backend RAII owns
  final device cleanup. Backend Stopped must mean the physical cleanup finished.
- IDs are local to one manager/run and records are not silently evicted. The
  default 256-record budget returns RecordBudget when full; production history
  and persistent idempotency require the M8 service/log policy.
- `query` is a snapshot. The control owner calls `tick` with current monotonic
  time/generation to advance expiration before publishing status.

Next: port the existing V4L2 I/O seam and capture session, add eventfd cancellation
and exact frame-drop counters, then connect CLI/ROS/Agent through one real router.
