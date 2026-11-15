# M0/M1 · Real Jetson baseline

2026-11-15 · upstream `45da40a8` · repair `afee249` · **simulated: false**.

Original upstream builds but its TensorRT 10 model loader crashes on this board.
The original failure is preserved. A narrow compatibility patch now runs fixed
images and the real USB camera. **This is a repaired baseline, not an unmodified
upstream inference pass.** No M2–M8 acceptance is implied.

## Results

| Experiment | Actual observation | Interpretation |
|---|---|---|
| Unmodified upstream configure/build | detectnet + video-viewer; exit 0 after missing dev package install | Native aarch64 build verified |
| v4l2-ctl capture | 1,800 frames; sequence 0–1799; 59.979771s driver timestamp span | Short tool baseline; no sequence gaps in this window |
| Driver intervals | 29.9934fps; P50 32.059ms / P95 36.007ms / P99 36.0161ms | Driver timestamp intervals, not inference or end-to-end latency |
| Original fixed-image inference | SIGSEGV / exit -11 after engine creation | Failed; GDB trace retained |
| Original headless viewer without output | exit 0 but zero frames / output creation failure | Failed; do not accept process exit alone |
| Viewer with explicit JPEG discard output | 1,785 output calls in supervised 60s run; exit 0 / normal shutdown | Actual capture and JPEG sink, no persistent images |
| Final repaired fixed image | two Banana detections, scores 0.987942 / 0.973741; exit 0 | Functional positive example; not accuracy evaluation |
| Final repaired USB inference | 1,712 inference reports and output calls; 60s requested; 60.6225s total process lifetime | Includes initialization, JPEG and verbose logging; no formal FPS claim |
| Restart after cleanup | 203 actual inference reports; 10s requested; exit 0 / normal shutdown | Reopen verified, not 100-cycle acceptance |
| Wrong output name | exit 1 and explicit missing-tensor error | Failure propagated, not an empty detection |
| GPU probe, all requested outputs | compiled; exit 0; correct shapes and finite probabilities | Real GPU enqueueV3 and completion verified |
| GPU probe, one auxiliary output | exit 0; correct scores and execution | Exercises full-I/O indexing vs requested-output indexing |

Final USB format readback is YUYV, 640×360, stride 1280, sizeimage 460800,
30/1 fps. Tool buffer flags identify MONOTONIC / SOE timestamps. No host dequeue
latency was collected by that tool; do not subtract a ROS/wall clock from them.

Only fruit classes are covered by this baseline model. A real scene with no fruit
can correctly yield no detections while inference still executes. The two boxes
in the fixed example are raw upstream outputs, not proof of calibrated detection quality.

## Evidence and verification

- [Manifest, run arguments, exit states and per-file hashes](../evidence/2026-11-15/manifest.json)
- [Full text evidence archive](../evidence/2026-11-15/raw-text-evidence.zip): 112 files;
  original compiler failures, SIGSEGV, GDB, failed first regression, repaired runs,
  per-process RSS/fd samples and environment snapshots are retained.
- [Model provenance and I/O manifest](../models/ssd-fruit-baseline.json)
- [Root cause and limits](debugging_journal.md)

The archive deliberately excludes weights, engines, binaries, private camera
images and credentials. The original unmodified binary/library remain board-local
in `projects/edgevision/artifacts`. Full raw runs remain on the board as well.

```bash
python3 - <<'PY'
import hashlib, json, pathlib, zipfile
p = pathlib.Path('projects/edgevision/evidence/2026-11-15')
m = json.loads((p / 'manifest.json').read_text())
assert hashlib.sha256((p / 'raw-text-evidence.zip').read_bytes()).hexdigest() == m['archive_sha256']
with zipfile.ZipFile(p / 'raw-text-evidence.zip') as z:
    for f in m['files']:
        assert hashlib.sha256(z.read(f['path'])).hexdigest() == f['sha256'], f['path']
print('all archived bytes verified')
PY
```

## Reproduce on the Jetson

Use the frozen personal branch and installed CUDA 12.6 / TensorRT 10.3. No JetPack,
kernel or CUDA upgrade is required. Stock CMake's apt/pip/PyTorch prebuild installer
is disabled. The observed missing dependency was installed with no existing
package upgrades:

```bash
sudo apt-get install -y --no-install-recommends --no-upgrade libgstreamer-plugins-bad1.0-dev
git submodule update --init utils ros c/plugins/pose tools/camera-capture
cmake -S . -B build-baseline -DBUILD_DEPS=OFF -DBUILD_INTERACTIVE=OFF -DCMAKE_BUILD_TYPE=Release
cmake --build build-baseline --target detectnet video-viewer -j2

python3 projects/edgevision/tools/fetch_baseline_model.py --out projects/edgevision/artifacts/m1-model
# Network-restricted board: download the official archive on the host, then use
# --archive /path/to/downloaded.tar.gz. Every artifact is checked against SHA256.
```

The original board clone's submodule objects were reused through command-scoped
local source URLs after GnuTLS network failures. The tracked gitlinks and upstream
`.gitmodules` were not changed. Training submodules are not needed for these targets.

Fixed image, real model and real output:

```bash
build-baseline/aarch64/bin/detectnet data/images/banana_0.jpg /tmp/banana-result.jpg \
  --model=projects/edgevision/artifacts/m1-model/ssd-mobilenet.onnx \
  --labels=projects/edgevision/artifacts/m1-model/labels.txt \
  --input_blob=input_0 --output_cvg=scores --output_bbox=boxes \
  --headless --log-level=verbose
```

Before opening the camera, verify its stable identity and that no other owner is
streaming. On this board the original daemon was active but IDLE; it was not
stopped or replaced. The tool uses the recorded by-id path; upstream discovery
was run with its resolved `/dev/video0`. Do not assume that node survives replug.

The camera does not advertise 640×480. Use actual 640×360 YUYV at 30fps. The next
example's run directory must be new, and its JPEG discard symlink is explicit:

```bash
task_root=projects/edgevision/artifacts
ln -s /dev/null "$task_root/m1-model/discard.jpg"  # create once, do not overwrite an existing path
python3 projects/edgevision/tools/run_baseline.py \
  --out "$task_root/my-camera-run" --seconds 60 --timeout 85 -- \
  build-baseline/aarch64/bin/detectnet /dev/video0 "$task_root/m1-model/discard.jpg" \
  --input-width=640 --input-height=360 --input-rate=30 --input-codec=raw \
  --model="$task_root/m1-model/ssd-mobilenet.onnx" --labels="$task_root/m1-model/labels.txt" \
  --input_blob=input_0 --output_cvg=scores --output_bbox=boxes --headless --log-level=verbose
```

The wrapper sends SIGINT, waits for ordinary cleanup, and force-kills only after
its documented limit. A force-kill or zero-frame run fails acceptance. This
wrapper does not implement the future capture state machine or automatic recovery.

GPU regression, against the built upstream libraries:

```bash
g++ -std=c++17 -O0 -g -Wno-deprecated-declarations \
  -Ibuild-baseline/aarch64/include -Ibuild-baseline/aarch64/include/jetson-inference \
  -Ibuild-baseline/aarch64/include/jetson-utils -I/usr/local/cuda/include \
  projects/edgevision/tools/trt_compat_probe.cpp -Lbuild-baseline/aarch64/lib \
  -Wl,-rpath,"$PWD/build-baseline/aarch64/lib" -ljetson-inference -ljetson-utils \
  -L/usr/local/cuda/lib64 -lcudart -lnvinfer -o /tmp/trt_compat_probe
/tmp/trt_compat_probe projects/edgevision/artifacts/m1-model/ssd-mobilenet.onnx full
/tmp/trt_compat_probe projects/edgevision/artifacts/m1-model/ssd-mobilenet.onnx auxiliary
```

## Not yet verified

30-minute acquisition; 100 starts/stops; physical unplug/replug; reference-framework
accuracy; CUDA numerical/stream optimization; kernel source build/patch/load/rollback;
ROS2 interface packages and real data; local VLM and model-driven Agent; RTSP in
this new mainline; production resource limits; 2h/12h endurance. `vision_msgs` is
not installed. Formal performance needs warmup exclusion, repeated measurements
and controlled power/temperature; none of the short-run numbers above substitutes.
