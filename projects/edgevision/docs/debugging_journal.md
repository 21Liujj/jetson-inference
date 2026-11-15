# Debugging journal

## 2026-11-15 · Original build missing WebRTC header

**Observed:** locked utils failed to compile gstDecoder/gstEncoder because
`gst/webrtc/webrtc.h` was missing. Raw `m1-build.log` and retry output are archived.

**Confirmed:** `libgstreamer-plugins-bad1.0-dev` was missing. A broad apt simulation
also proposed libsoup upgrades, which were not needed. Actual install used
`apt-get install -y --no-install-recommends --no-upgrade libgstreamer-plugins-bad1.0-dev`:
two new packages, zero upgrades/removals. The unchanged source then built both
detectnet and video-viewer. This is dependency repair, not personal driver work.

## 2026-11-15 · TensorRT engine builds, loader SIGSEGV

**Observed:** original source `45da40a8` built an FP16 SSD engine, then exited -11
while initializing its first output. GDB records SIGSEGV inside
`tensorNet::LoadEngine`. See `m1-fixed/run.json`, `m1-fixed/stdout.log` and
`m1-fixed-gdb.log` in the raw archive.

**Confirmed by source and repair regression:** a local execution `context` was
created, but output binding called member `mContext` before its assignment at the
end of initialization. The member was initialized to null in the constructor.
Also, enqueueV3 needs named I/O addresses; keeping a populated legacy binding
array does not bind the input for enqueueV3.

**Adjacent contract defects:** the TensorRT 10 output branch retained the explicit
batch dimension while the SSD consumer expected nonbatch dimensions. An auxiliary
binding used an index from the full engine I/O list to index the shorter requested
output-name list. Both are corrected in the same narrow loader patch.

**Second regression failure retained:** initial real-GPU probes rejected the CHW
shape contract. `copyDims` copied only logical axes into a default-constructed
Dims3 and left its remaining axis unset. Initializing remaining dimensions to 1
and correcting the shift bounds made both probes pass. Original failing `full.log`
and `auxiliary.log` remain alongside `*-retry.log`.

**Scope:** `afee249` changes the upstream main repository, not a dirty submodule.
It is a compatibility repair, not a CUDA speed optimization. The successful path
is static batch 1 with FP32 I/O / FP16 compute for the frozen SSD model. Dynamic
shapes, arbitrary I/O types, repeated LoadNetwork and all initialization-failure
cleanup paths are not validated by this patch. Auxiliary buffer lifetime in the
legacy loader still needs a full ownership audit before a persistent execution layer.

## 2026-11-15 · Exit 0 with no camera frames

**Observed:** `video-viewer /dev/video0 --headless` without an explicit output URI
returned 0 in 0.5s, but logged output creation failure, produced no frames and did
not print normal shutdown. This run is retained as `m1-upstream-camera`; it fails
acceptance despite process exit 0.

**Confirmed:** `videoOptions::Parse` rejects the missing headless output before
the later null-output fallback. To keep utils unmodified, subsequent runs use an
explicit JPEG output path symlinked to `/dev/null`. They still perform real JPEG
encoding; they save no private camera images and are not pure capture benchmarks.

## 2026-11-15 · Access and tool distinctions

- Board GitHub submodule cloning returned GnuTLS -110; local object sources with
  the exact required gitlinks succeeded. This is a network symptom; its network
  root cause is unproven.
- Windows curl to the public model timed out; Python download from the same
  official link succeeded and verified the archive hash, then SFTP transferred
  the exact ONNX. No empty fallback model was created.
- Initial `trtexec --version` exited 1; its version banner and dpkg prove the tool
  exists. The collector now uses `--help` and records absolute tool paths.
- `ros2` runs after sourcing Humble; `ros2 pkg prefix vision_msgs` reports missing
  package. Installation and integration remain separate milestones.
- A diagnostic root redirection to a preexisting user-owned `/tmp` file was denied;
  direct apt simulation avoided that path. It did not indicate authentication failure.
- Windows checkout case collisions are not intentional upstream source edits and
  are not staged. Upstream c/tensorNet.cpp stores CRLF; that byte convention is
  preserved with a scoped Git attribute to avoid a whole-file formatting diff.

## Interview questions for this checkpoint

1. Why does a valid engine not guarantee the execution context can run it?
2. What does enqueueV3 return, and when can its input memory be reused?
3. How do `[1,3000,9]` and `[3000,9]` differ for the legacy SSD postprocessor?
4. Why is exit code 0 insufficient for capture acceptance?
5. Why do zero V4L2 sequence gaps not prove zero application drops or latency?
6. What work does the JPEG discard output add to these measurements?
