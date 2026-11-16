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
| EV-CAP-003 | 运行中拔插与 stop 的优先级 | 真实恢复与停止后插回分别通过 |
| EV-METRIC-001 | 未知指标、时钟和跨会话污染 | 有界窗口与 generation 隔离已测 |
| EV-TEST-001 | 人工拔插发生在测试窗口结束后 | 原始不通过保留，延长窗口与驻留观察后通过 |
| EV-BUILD-002 | 新 target 未配置、错误 build 路径及 dmesg 参数 | 保留原始错误，显式配置/核对路径与参数后成功 |
| EV-GPU-002 | 对照运行的 engine 配置漂移与 workspace 记录 | 原 R6/R7 保留，R8 对齐 M1；更正 manifest 的未实证内存上限 |

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


### EV-OPS-001 · 2026-11-16 Windows 客户端 NoState 续记

本轮重连时电脑 Tailscale service 为 Running、隧道网卡 Up，但 CLI 报 NoState，
Health 为 Starting，尚无 tailnet IP；进程列表无 tailscale-ipn.exe。仅启动已安装的
用户 GUI 客户端后，状态转 Running、Jetson peer Online，SSH 实际登录成功。
没有重装、改认证信息、清 state 文件或重启服务；当前观察支持本次客户端会话未就绪，
不据此断言所有 NoState 都同因。重新确认板端 L4T、稳定相机标识、旧服务 IDLE 与
执行文件哈希，均同前一轮。新后端先按文档做 60s，再扩大到 30min。


## EV-CAP-003 · 2026-11-16 · 运行中恢复与停止的控制边界

本机开发中把“启动重试”扩展为已建立会话的运行中恢复；原来的启动验收不能作为
拔插恢复证据。新策略由控制 owner 在旧 Stopped 后调度重开，每次增加 generation，
保留已完成 start 请求的历史结果，不把旧任务改写成新会话的结果。

恢复尝试总预算按一次显式 start 保留，成功一次不补充预算，防止反复掉线形成无限重连；
另设单次恢复时间预算与有界退避。仅稳定 by-id/by-path 允许自动恢复；videoN 即使
显式允许打开，也不会在热插拔后自动假定仍是同一设备。后端按真实 errno 区分启动
瞬态错误与运行中设备消失，非法 payload/槽位状态等程序契约错误不盲目重开。

本机回归覆盖旧会话尚未清理、新会话未收到首帧、旧事件迟到、停止在退避期间、
停止在故障清理期间、恢复超时、次数耗尽、成功后再次掉线，以及排队的冲突 start
不能吞掉自动重开动作。用户显式 stop 确认之前的采集故障；后续新的清理错误仍失败。
R4 Windows 7/7、目标 sanitizer 9/9 通过。真实拔插中 gen1 报 POLL ENODEV，
4 次有限重开后 gen5 有效首帧恢复，检测故障到恢复为 12.3811s；参数为 5 次/30s，
不能推广到默认 3 次/15s。R5 真实故障后显式 stop，节点重新出现后进程继续观察 5s，
保持 IDLE、generation=1、无活动 worker/设备资源。两个监督运行均正常退出。
[原始源码、日志、失败和复测](../evidence/2026-11-16-runtime-recovery/summary.json)。
未扩展内核或改变 USB 配置；UVC 控制查询警告仍存在，底层原因未确认。

面试追问：为什么恢复成功不补充次数？已完成 start 历史为什么不能改写？stop 如何
抢在退避重开之前生效？如何证明“没有重开”发生在控制进程仍存活的时候？

## EV-METRIC-001 · 2026-11-16 · 未知指标、时钟与会话隔离

新增 HealthMonitor，记录有界时间窗口与样本量。捕获率来自有效 DQBUF 累计数
（包含因无自有槽位而丢弃的有效帧），消费延迟与结果完成延迟分别采样。没有推理
完成样本时结果分位数为空，不填 0；只报 host dequeue 到观察/结果，不混同 CUDA
事件耗时或 SOE 曝光起点到用户态的交付时延。

generation 切换清空旧窗口；新 generation 未采到足够数据时保持未知。后端队列与
采集计数来自不同短锁快照，活跃采样不强行做逐项相等检查；停止后的最终计数才用于
严格守恒核对。遥测过期与“遥测活着但未观察到帧数推进”分别诊断。

本机测试覆盖容量/时间窗口、分位数、单样本无 FPS、未知资源、旧帧结果拒绝、
计数倒退、切代与并发一致性；窗口 P99 是运行诊断，不是正式性能 benchmark。

R4 目标 sanitizer 测试通过，真实 recovery probe 输出 health.csv。该工具不执行
模型，因此 result_n 保持 0、result_p95_ns 为空；不能拿消费率当模型吞吐。

## EV-TEST-001 · 2026-11-16 · 无效人工操作窗口与阴性结果

第二次人工拔插前，R4 probe 的 180s 等待已经结束；日志没有运行中断连，summary
为 RECOVERY_NOT_OBSERVED，退出 1。用户回复已操作不能替代事件日志，因此本轮不通过。
检查还发现原 stop 观察只有 500ms，短于 1s 首次重试，也可能在插回前进程就退出，
不足以证明显式停止抑制重开。原始失败、监督与事件保留，不覆盖。

R5 仅调整测试工具：人工窗口上限 600s；故障后 stop，等待观察到设备缺失再实际返回，
进程仍活着、控制 owner 持续 poll，并在节点返回后额外观察 5s。60s 内未重新枚举则
明确 POST_STOP_REPLUG_NOT_OBSERVED。本次实际观察约 12.77s，节点返回后 5s 持续
IDLE，退出 0。阴性结论需要有活跃的观测者与足够窗口，不能以进程死掉后“没重开”充数。

## EV-BUILD-002 · 2026-11-16 · 工具配置与命令错误

R4 增加 recovery target 后，旧 Makefile 一次请求两个目标：先构建 capture 时触发
重生成，随后仍报新 target 无规则。显式 cmake 配置并重跑后成功；日志支持配置时序
问题，未据此断言 CMake 内部根因。R5 首次误用了不存在的 build/jetson-native，
核对实际 capture-native 后成功构建。两次失败日志均保留。

只读采集内核日志时使用了本机 dmesg 不支持的 --time-format=raw。且未设置 pipefail
时 tail 成功掩盖了上游失败，表现为 exit0、stderr 报错、stdout 空。保留该次记录，
改为默认单调时间戳输出并启用 pipefail 后成功。诊断命令必须检查 stderr 与实际内容，
不能仅凭退出码。没有更改内核、权限设置或日志配置。

## EV-GPU-002 · 2026-11-16 · 相同 ONNX 不代表相同 engine

R6/R7 个人输入适配直接调用 DetectNet::Create 时，把 allowGPUFallback 设为了 false；
M1 CLI 使用默认 true。冻结上游 cache_prefix 包含该标志，因此首次 R6 创建了
`.1.0.100300.GPU.FP16.engine`，SHA256 为
e2e26b99fdc5a1ab7f2461a35acac997cb7b15bf025728c0e268a2db3a1a677d。
R7 加载该缓存，60s 完成1,802次。它是有效功能测试，但不能假称使用 M1 的同一 engine。

R8 只对齐该标志为 true，重用 M1 `.1.1.100300.GPU.FP16.engine`，已在启动前冻结
期望 SHA256 3cb11235bc6338895bca58e710383dd36c7ecfded68ee3491ec20ddd7eab2fc4；
验收同时核对实际加载日志和运行后哈希。旧 engine 与原始运行不删除，不混合版本结果。
输入接口、代码版本、builder 参数与 engine 字节都要记录，相同模型哈希不足以证明
比较条件相同。当前没有正式性能或数值改进结论。

R8 实际复测：60s 窗口完成/发布1,802次，总进程63.1163s，正常退出；最终
IDLE/UNLOADED、pool_busy=0、视频无owner。加载日志及运行后SHA均核对为M1原engine。
本次只有有效空检测结果，精度未验；原R7的1,802次也不与本轮合并成一个长窗口。

审计还发现模型 manifest 把 workspace 写成2GiB，没有区分日志请求与实际生效。
原始 M1 `m1-fixed/stdout.log` 和 R6 构建都记录536,870,912字节；冻结 tensorNet.cpp 的
TRT10 分支并未调用 setMemoryPoolLimit，旧 setMaxWorkspaceSize 仅位于 <10 分支。
因此将 manifest 改为实际观察的 requested 值、explicit limit=false；实际默认上限未测，
不能声称 workspace 被限制到512MiB或2GiB。本轮只纠正记录，不改变上游 builder 行为。

代码审查另发现正常退出时模型已由 worker 销毁，但首次 probe 的最后快照仍报 READY。
R7 在销毁后发布 UNLOADED，控制线程 join 后再采最终状态；真实 R7 窗口最终
capture=IDLE/model=UNLOADED。模型加载失败保留 ERROR，且不改写正常采集为故障。
详见 [输入适配与证据](m2-inference.md)。
