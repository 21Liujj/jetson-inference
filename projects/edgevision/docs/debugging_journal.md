# Debugging journal

本文件是工程问题和经验的唯一正文；阶段报告、技能和续施工记录引用本文件，
不另开一套故障日志。按稳定 ID 或关键词检索，避免每次加载全部历史。

| ID | 领域 / 现象 | 状态 |
|---|---|---|
| EV-BUILD-001 | 缺少 WebRTC 开发头文件 | 已解决 |
| EV-GPU-001 | 原版 TRT10 loader SIGSEGV、I/O 与维度契约 | 受支持模型路径已修复，泛化边界未验 |
| EV-CAP-001 | headless 缺输出时 exit0 但无帧 | 复现，基线使用明确输出绕过 |
| EV-ENV-001 | 下载、工具路径、ROS 包与 Windows 大小写碰撞 | 分项结论见原条目 |
| EV-BASE-001 | 30 分钟早期 RSS 上升后稳定 | 已记录，原因未证实 |
| EV-CTRL-001 | 首帧、generation、取消与物理清理边界 | CPU/目标测试覆盖，硬件分项验收 |
| EV-CAP-002 | 第 38 次启动 S_FMT EIO / UVC probe -75 | 原始失败保留，100 轮受限恢复复测通过，根因未确认 |
| EV-OPS-001 | 搬回家后 Jetson Tailscale 离线 | 连上当前 Wi-Fi 后 SSH 恢复 |

新增问题记录：ID、时间/环境/源码、触发与预期/实际、复现、原始证据、已确认原因、
假设、处理、复测、未解决项和面试追问。后续结论追加到原 ID，保留失败事实。

## 2026-11-15 · Original build missing WebRTC header

ID: **EV-BUILD-001**

**Observed:** locked utils failed to compile gstDecoder/gstEncoder because
`gst/webrtc/webrtc.h` was missing. Raw `m1-build.log` and retry output are archived.

**Confirmed:** `libgstreamer-plugins-bad1.0-dev` was missing. A broad apt simulation
also proposed libsoup upgrades, which were not needed. Actual install used
`apt-get install -y --no-install-recommends --no-upgrade libgstreamer-plugins-bad1.0-dev`:
two new packages, zero upgrades/removals. The unchanged source then built both
detectnet and video-viewer. This is dependency repair, not personal driver work.

## 2026-11-15 · TensorRT engine builds, loader SIGSEGV

ID: **EV-GPU-001**

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

ID: **EV-CAP-001**

**Observed:** `video-viewer /dev/video0 --headless` without an explicit output URI
returned 0 in 0.5s, but logged output creation failure, produced no frames and did
not print normal shutdown. This run is retained as `m1-upstream-camera`; it fails
acceptance despite process exit 0.

**Confirmed:** `videoOptions::Parse` rejects the missing headless output before
the later null-output fallback. To keep utils unmodified, subsequent runs use an
explicit JPEG output path symlinked to `/dev/null`. They still perform real JPEG
encoding; they save no private camera images and are not pure capture benchmarks.

## 2026-11-15 · Access and tool distinctions

ID: **EV-ENV-001**

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

技能校验器初次运行缺少 PyYAML；依赖隔离安装到工作区 `.tools/python` 后，
8 个仓库领域技能通过官方 quick_validate。未修改系统 Python 或全局技能配置。

## Interview questions for this checkpoint

1. Why does a valid engine not guarantee the execution context can run it?
2. What does enqueueV3 return, and when can its input memory be reused?
3. How do `[1,3000,9]` and `[3000,9]` differ for the legacy SSD postprocessor?
4. Why is exit code 0 insufficient for capture acceptance?
5. Why do zero V4L2 sequence gaps not prove zero application drops or latency?
6. What work does the JPEG discard output add to these measurements?

## 2026-11-15 · 30-minute follow-up and shutdown checkpoint

ID: **EV-BASE-001**

The repaired upstream path completed 53,898 inference/output calls over the full
supervised 30-minute window and normal cleanup. Evidence is archived separately
so the first 112-file baseline package is immutable. First-to-last resource-window
RSS medians rose by 44,668 KiB before a flat last 20 minutes; cache/lazy allocation
is a hypothesis, not a confirmed cause. FD samples stayed within 52–53. No formal
FPS/P99, zero-leak or multi-hour claim is derived from this run.

M2 CPU-core source is separately verified on Windows and native Jetson with
sanitizers, 4/4 each. Those tests use explicit CPU fence fixtures and are not GPU
or real V4L2 acceptance. A raw-log Git attribute preserves CRLF bytes in Windows
evidence so committed files still match recorded hashes.

The user was told the camera was released and all evidence was saved locally,
then advised to use normal shutdown before removing power. No shutdown command
was executed by the assistant; physical power state must be rechecked next time.

An additional committed-byte check found that three Windows text logs were
already staged with LF before the raw-evidence attribute was added. Ordinary
git add reused the cached index entries. `git add --renormalize` under the scoped
raw attribute restores their original CRLF bytes; checks verify both index/Git
blob hashes, not only local file hashes. Executed tests/results did not change.

## 2026-11-15 · Local control routing after board shutdown

ID: **EV-CTRL-001**

The typed router is tested on Windows with an explicit CPU event fixture, not
a virtual camera masquerading as hardware. No SSH or board run is performed.

Design review exposed three boundary problems before real-backend integration:

1. Start increments the capture generation. Comparing its active task against
   the original request generation would expire its own execution as stale.
   A separate execution_generation advances with the start while the original
   request remains unchanged for idempotent retries. Regression checks both.
2. A stop queued behind a no-frame STARTING task could wait for its whole start
   deadline. The control owner now cancels the start and requests stop on its
   next poll; both tasks retain the physical cleanup dependency until Stopped.
3. Cancel and first-frame publication need a common serialization point. Router
   cancellation now takes the same mutex as the first-frame transition. If
   cancellation wins, a late frame cannot publish RUNNING; if completion wins,
   canceling the already-completed start does not stop an established session.

Backend start/poll calls execute outside that mutex. A start exception is treated
as possibly partial acquisition, and cannot pretend no resources remain.
Only the backend's Stopped event clears physical ownership. The eventual Linux
backend must prove that acknowledgement follows actual cleanup; these CPU tests
cannot establish that device-level fact. See the new Windows evidence package.

Interview checkpoint: explain accepted vs RUNNING vs task completion; why a
logical timeout cannot release physical memory; why request identity and the
execution generation differ; and how a stop interrupts a no-frame poll without
another thread closing a camera fd being used by its owner.

## EV-CAP-002 · 2026-11-15 · 连续启停时 S_FMT EIO

**环境与触发：** Orin Nano / L4T 36.4.3 / 5.15.148-tegra，capture-r2 原生 Release，
稳定 by-id 路径，YUYV 640×360，4 个驱动 buffer、4 个自有 slot、READY 队列 2。
每轮收到 3 帧后停止，立即下一轮。源码输入 ZIP 的哈希随证据保存。

**实际与预期：** 正常 10s、慢消费者 10s、持有 4 个 slot 的 5s 测试通过；
100 次连续启停在完成 37 轮后，第 38 次启动报 `S_FMT: Input/output error`。
原 ledger 保持 `complete=false`，不是 100 次通过。已完成轮次 fd 均回到 6、
视频映射为 0；这是这些窗口的资源证据，不能因此完全排除所有生命周期问题。

**关联证据：** 同一时段内核有 `Failed to set UVC probe control : -75 (exp. 26)`。
稍后独立重开成功，2s 交付 61 帧。无设备和忙设备另行验证返回明确失败，
忙设备测试中原 owner 继续正常采集。原始 R2 日志、CSV、输入源码和后续 R3 记录
分别归档到 `evidence/2026-11-15-m2-capture/`，不覆盖原始失败。

**已确认与推断：** 已确认失败发生在格式协商 ioctl，并有 UVC probe 控制传输错误。
[Linux v5.15 参考源码](https://github.com/torvalds/linux/blob/v5.15/drivers/media/usb/uvc/uvc_video.c)
在该控制传输返回值不符合期望时收敛为 EIO；
[V4L2 格式协商参考](https://github.com/torvalds/linux/blob/v5.15/drivers/media/usb/uvc/uvc_v4l2.c)
说明其调用路径。它们是参考，不是已核对的 NVIDIA BSP 源码。
相机固件、链路、电源或驱动中的具体根因仍未确认，不能声称修复 UVC 缺陷。

**处理：** 增加按 errno 分类的启动恢复，仅 EIO/ETIMEDOUT/EOVERFLOW 可重试。
默认最多 2 次，退避 250/500ms；控制 owner 等到真实 Stopped 后进入 RECOVERING，
每次重新启动增加 generation 并 retarget 执行任务，保留原 request_id 身份。
取消、stop 和 deadline 可以结束退避；busy/无设备/非法配置不盲目重试。
CPU 测试覆盖先清理后重试、旧 generation、预算耗尽、退避取消和超时；
目标新增“首次 S_FMT EIO 后成功”的显式 syscall 夹具。

**R3 复测：** 真实 100 轮、300 帧通过，共发生 3 次受限启动重试；各轮停止后 fd=6、
视频映射=0。内核仍有对应 UVC 控制错误，说明恢复没有修复底层根因。

**尚未完成：** 不以夹具证明其他物理恢复场景；
运行中断连重连和 30min 新后端长采集仍需独立验证。未改内核、quirks 或重置 USB。

**面试追问：** 如何区分 EIO 的发生层次和根因？为什么重试前先清理、每次重开换
generation？为什么不能让采集 worker 自己无限重试？如何证明慢消费者的主动淘汰
不同于 driver sequence 缺口？

## EV-OPS-001 · 2026-11-15 · 重上电后 SSH 不通

**现象：** SSH 超时，电脑 Tailscale Running，Jetson peer Offline，最后在线 22:33。
用户确认重新接入当前 Wi-Fi 后，Tailscale peer Online，SSH 实际登录成功；相机稳定
设备标识与 L4T 保持一致，旧服务 IDLE，视频节点无占用。

**处理与边界：** 先区分网络不可达与认证失败，没有修改密码、sshd 或 Tailscale 配置。
连接恢复与用户连网时间相关，但没有板端离线时的日志，不能进一步断言内部断网原因。
重上电后重新核对软件、设备和 owner，不把先前可访问当作当前可访问。
