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
| EV-CTRL-002 | 检测与启停共享任务边界、停止抢占及 GPU 在途取消 | R10 主机9/9、目标11/11、真实GPU与联合拔插通过 |

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

## EV-CTRL-002 · 2026-11-16 · 单一任务记录与物理执行确认

把新执行器接到已有 ControlRouter/TaskManager，不能为 CLI 检测另建一套任务编号与
终态。执行器只维护一个物理 job/completion 槽；逻辑状态、幂等历史、deadline 和结果
预算仍由唯一 TaskManager 维护。backend initialize/submit/query/collect/cancel_pending/
shutdown 都在 router 锁外调用；GPU worker 独占模型与 lease，控制线程不析构在途帧。

**停止优先级：** 一个 GPU 任务加一个等待检测会填满原队列。有效 stop 可取消等待项
后受理；它不能借此回收运行中的执行槽。已经过期、非法 generation 或冲突 request_id
的 stop 不得清掉别人的排队任务。受理停止后拒绝新工作，幂等重试仍返回原记录。
控制 owner 先请求采集停止并使当前检测逻辑取消，等 GPU 完成、capture Stopped 和
物理完成记录到齐，才把 stop 记录置 DONE。新的清理错误必须让 stop 失败，不被它
“确认旧故障”的语义掩盖。shutdown 同样异步排空，物理状态确认前不报完成。

**原始测试失败：** 首次新跨 generation 测试使用1万纳秒 deadline，却把虚拟时钟
推进到10亿纳秒以后，实际应先 EXPIRED；不能期望后来覆盖成 STALE_GENERATION。
该失败源码与日志保留，改为足够长的 deadline，另保留独立超时用例。CPU 夹具的
cancel_pending 也需允许“物理完成已发生、取消通知稍后到达”的幂等空操作。

**R9 实机：** 七项场景正常退出；6次真实上游 GPU 推理、4次显式 CUDA 测试负载。
在 EXPIRED/CANCELED 时 cudaEventQuery 仍为 not-ready，execution_pending=true，
结果为空；完成后逻辑终态不变、physical pending清除。停止窗口 READY 队列归零，
唯一在途 lease 仍占pool；之后视频映射归零并可换generation再次检测。
shutdown 结束为 IDLE/UNLOADED、pool_busy=0、video_maps=0。200ms为测试负载请求，
不作为性能或时延改进结论；夹具不冒充这些真实 GPU 记录。

**R10 收尾：** 为证据记录增加实际 GPU 提交时间与任务deadline，要求在过期前先看到
RUNNING和真实event pending；避免只观察到“过期后才 launch”。将测试CUDA target
显式设为 Shared runtime；R9链接命令曾同时列出cudart.so与cudart_static，虽然场景
通过，不能把这一混合设置悄悄带入后续对照。保留原始构建与R9结果，R10单独复测通过7项；模型缺失负例也通过。

真实联合拔插中，故障发生时CUDA event仍not-ready，READY队列0、在途pool1。
原任务775等待物理完成后返回FAILED/CAPTURE_NOT_RUNNING且无结果；3次有限重开至
gen4，两个新会话任务DONE。检测故障到首帧7.5720s，最终776次GPU提交/完成，
IDLE/UNLOADED、pool0/maps0、正常退出。使用5次/30s恢复及4096条有界测试历史。

**构建环境续记 EV-ENV-001：** 本机有nvcc但非登录SSH的PATH不含cuda/bin。
FindCUDAToolkit找到了绝对路径，enable_language(CUDA)首次仍得到COMPILER-NOTFOUND。
显式传入 -DCMAKE_CUDA_COMPILER=/usr/local/cuda/bin/nvcc 后成功，不重装或升级CUDA。
原错误及修复配置日志分开保存。

**未验与追问：** 不可恢复CUDA故障仍采取进程隔离，未做破坏性故障注入；任务历史
默认256条，达到预算明确拒绝，长期服务的持久化/保留政策属于M8。为什么逻辑完成与
物理完成不能合并？stop满队列时如何受理且保持幂等？如何证明过期之前GPU已经提交？

## EV-KERNEL-001 · 2026-11-16 · 匹配源码构建与运行模块身份

当前 headers 包没有 UVC C 源码，不能只用 headers 宣称已完成驱动开发。下载对应
R36.4.3 官方 source archive，冻结两层归档和13文件哈希，再用目标真实 config/Symvers
原生构建。GCC11.4与原内核11.3有差异；实际vermagic/srcversion及155导入CRC全部匹配，
未修改headers配置。该事实仅适用于本次目标，不泛化为任意编译器或内核均可混用。

模块能编译仍不足以证明可运行。先备份系统模块并核对哈希，生成可执行回退脚本和
具体切换计划；再实际进行系统原模块→自编译原版→系统原模块三段采集，均正常退出，
分别151/150/151帧、无sequence缺口。每段用sysfs GNU Build ID核验实际加载对象；
默认modinfo读取磁盘文件，不能据此证明insmod候选已运行。原文件未被覆盖。

签名强制当前为N，未改变策略。加载未签名模块使taint4096→12288；卸载和回退不会清除
taint，不能把“系统原模块已恢复”写成“内核状态完全恢复”。保留实际值，不因此擅自重启。
UVC控制查询告警仍存在，无证据将其归因为本次代码或声称修复固件。

面试追问：为什么vermagic匹配仍需符号CRC与实机验证？如何确认真正运行的是新模块？
为什么trace应位于异步复制最后引用释放处，而不只放在USB EOF？回退能恢复哪些状态？

## EV-KERNEL-002 · 2026-11-16 · 内部回收、消费淘汰与不同会话编号

先保存系统已有V4L2/vb2追踪，再在UVC最后复制引用释放、内部损坏帧重排及取消归还处
增加私有状态/原因和stream ID。新增字段必须来自仍有引用的对象，metadata队列不能
误用capture的container_of；不新增IRQ锁序或包级printk。补丁导入172个符号CRC匹配。

R2慢消费者8秒：驱动交付/复制241帧，消费80，READY替换159，停止清队列2，
满足241=80+159+2；80条消费记录全关联。另有sequence0、668字节、error1的内部回收，
没有对应DQBUF。恢复场景也出现长度460800但error1的内部回收；不能仅用长度判好帧。
这些错误的更底层成因未采逐包证据，未声称修复驱动或固件。

真实拔插7.3799s恢复，用户generation1→4，内核stream ID1→2：失败的open尝试与实际
STREAMON生命周期不同。按USB路径、sequence与同域单调时间建立明确映射；有歧义、
负延迟或trace loss时拒绝验收。1803条消费全部关联，原版实际加载与采集回退通过。
R1矩阵同时有engine构建；R2重测保存CPU和/proc/stat，但未单独记录冻结功耗/频率条件，不能声称零开销。

首次工具调用把source ZIP文件名写错，且sudo创建的新kernel目录导致普通SFTP写入拒绝。
核对真实m3-uvc-pristine-input.zip，修正仅该新目录owner，使用新的R2构建目录；
该次预检失败保留，未加载错误模块。问题来自工具路径/权限，不归因为内核源码失败。

面试追问：为什么241帧里只消费80帧不是驱动丢了161帧？内部回收为什么没有DQBUF？
为什么内核stream2对应用户generation4？如何证明trace本身没有丢事件？

## EV-EVIDENCE-002 · 2026-11-16 · 汇总文字不能覆盖原始事件

整合脚本的静态限制文字曾写“未观察到内部损坏帧回收”，但同一汇总计数和原始trace
明确有reason1事件。发布前逐项对照发现冲突，保留本机初版归档哈希，纠正派生汇总；
原始trace/CSV字节没有修改。不能只检查“脚本exit0”或复制模板结论，必须核对真实计数、
原始样例与文字陈述一致；该事件也不能被扩大解释为已经定位并修复了USB底层缺陷。

## EV-GPU-003 · 2026-11-16 · 小分数偏差与离散检测变化

冻结旧YOLOv8n ONNX的metadata、I/O与后处理，先登记200张子集AP下降≤0.5个百分点、
24张固定图高置信框匹配率≥98%等门槛。参考使用ORT CPU；目标新建FP16-enabled engine，
不是继承旧engine结果。实际AP下降0.046895个百分点，匹配199/200；两项通过。
唯一漏匹配框参考分数0.252635，目标0.249390，跨0.25阈值。原输出和样例保留。
不能因为tensor数值偏差小就推导检测框完全一致；阈值、排序、NMS会放大局部变化。

CPU letterbox24例（含奇数尺寸、纯色、17字节行padding）与PC输出逐字节相同；
原始输出R1与R4完全一致。视频60秒1803次均为空检测是有效执行，不是精度证明。
M3继续使用GPU颜色转换→CPU letterbox→GPU tensor的串行路径，M5才优化往返和异步依赖。

## EV-TEST-002 · 2026-11-16 · 重启不保证只增加一个generation

R4 YOLO任务验收的正常、过期、取消、stop均通过，重启后却因断言`new==old+1`失败。
该断言忽略已有合法启动重试；每次重新打开都会增加generation。窗口内核日志也出现
既有UVC probe -75，未据此编造驱动新缺陷或改变生产重连策略。

保留原失败源码、failure/events及监督日志，测试改为记录start_retries并严格核对
`new>old && new-old==1+start_retries`。R5 YOLO和原SSD各7项通过，最终物理资源排空。
这是修正验收的不成立假设，不是放松GPU event、lease、取消或超时要求。

## EV-GPU-004 · 2026-11-16 · 插值一字节误差仍需模型回归

上游YUYV颜色转换继续复用，个人CUDA kernel只融合RGB letterbox、CHW与归一化。
OpenCV CPU插值有整数舍入，CUDA浮点双线性可能相差一个u8值。首轮前冻结max≤1/255+1e-6、
mean≤0.001；48例通过，实际max0.003921598196，最大mean0.0004806570448。
不能把这写成byte-exact。继续跑原200张COCO子集，AP下降0.0652031个百分点，框匹配99.5%，
独立输出解析无失败；满足未放松的原模型门槛。pinned/mapped/P2原始输出相同。

原路径每帧约3次cudaMemcpy、2次device-wide同步。融合后保留producer→preprocess→inference
实际事件依赖；单context在前一event完成前拒绝重绑定。真实摄像头1802次推理、7项共享任务
GPU取消/过期/stop/shutdown通过；物理完成前继续保留lease，不能用逻辑终态释放输入。

## EV-GPU-005 · 2026-11-16 · 吞吐提高不意味着尾延迟同时最优

同模型与固定RGB输入，3轮×1000次，P0中位73.76images/s、P99=14.852ms；P1-pinned为
129.99images/s、11.491ms；P2-pinned为190.17images/s、14.566ms。P2较P1吞吐提高而P99变差，
保留队列与CPU后处理代价，运行时选择P1。不能把离线190images/s写成USB摄像头190fps。

Nsight实际GPU区间显示597/600次预处理与另一stream的TRT kernel相交，但仅占预处理GPU时间
8.66%，不是端到端收益百分比。profile本身使P2降至142.74images/s，因此正式性能来自无profiler
矩阵。P0探针GPU阶段event零列只是未测占位，派生表标为unknown，不解释为GPU零耗时。
板端concurrentManagedAccess=0；managed support与CPU/GPU可并发访问并非同一个能力。

M4曾把未采集功耗状态误写为“DVFS未锁定”。M5查询发现板上已是固定CPU/GPU/EMC状态；
不能据此倒推M4测试时的状态。更正为“未单独记录冻结条件”，原M4记录不变，开销结论仍有限。

## EV-VLM-001 · 2026-11-16 · 可执行文件能启动不代表模型运行环境完整

审计旧EdgeQwen资产，实际config/权重是Qwen3-VL-2B，旧README的Qwen2文字不作为身份依据。
权重SHA与本地HF revision的LFS元数据吻合，engine、plugin、CLI与图片分别登记哈希；旧性能
和稳定性结论不直接继承。首个新工作目录运行因默认build/libNvInfer_edgellm_plugin.so相对路径
失败，原日志保留。第二轮仅给该子进程设置EDGELLM_PLUGIN_PATH绝对路径，未改全局环境、
未修改旧工程；真实图片问答正常退出。它仅证明独立VQA，尚不代表Agent工具闭环或组合内存验收。

## EV-ROS-001 · 2026-11-16 · 构建进程的Python隔离与ament链接约定

原生colcon首轮生成接口失败，调用链落到用户目录setuptools，而系统packaging不支持
strip_trailing_zero参数。保留R1日志，仅在构建环境设置PYTHONNOUSERSITE=1，未升级或卸载
用户Python包。三个package.xml显式声明ament_cmake build_type。
R2又因ament_target_dependencies使用plain target_link_libraries而应用先用了PRIVATE签名失败；
将应用target统一为plain，核心库仍保留其原有target作用域声明。R3三包原生构建通过。
安装成功与运行通过分开，下一项实际暴露了独立的动态库问题。

## EV-ROS-002 · 2026-11-16 · 安装产物加载旧动态库导致已修复缺陷重现

首轮真实ROS服务测试超时，子进程SIGSEGV。GDB定位在/usr/local/lib/libjetson-inference.so的
IExecutionContext::setTensorAddress。readelf/ldd显示构建目录有验证前缀RUNPATH，ament安装
产物却没有；实际加载的是SHA555385b4…旧系统库，非SHAa2ea6868…修复版。头文件和链接时
绝对路径均不能证明运行时加载对象，不能将它误判为ROS服务或新CUDA kernel缺陷。

应用设置明确INSTALL_RPATH，并在初始化模型worker前用dlopen(RTLD_NOLOAD)/dlinfo核对
实际加载库的canonical路径。修复后真实topics、服务、Action、GPU在途取消/超时与bag回放
通过；子进程显式污染LD_LIBRARY_PATH时以错误码1拒绝启动，未再进入旧loader。
原GDB/失败二进制来源、库哈希和两轮复测保留；系统库及全局loader配置未改动。

面试追问：DT_NEEDED、RUNPATH和LD_LIBRARY_PATH各影响什么？为什么build成功但install崩溃？
路径校验能证明哪些身份，为什么正式证据仍需哈希？部署时如何避免用系统旧库覆盖自己的修复？

## EV-ROS-003 · 2026-11-16 · 任务周期、FIFO和结果帧龄

四组20秒初探都约30次完成/秒，ROS关闭P95约8.12ms，诊断/5Hz/30Hz约62.30/42.91/57.01ms。
后者是host DQ到核心结果完成，不含DDS接收；不能把差值直接称为DDS延迟或GPU推理时间。
ROS封装以周期任务消费既有FIFO READY队列，直接基线则随帧消费。队列容量虽有界，仍可能
保留较旧帧；需要按时间线区分排队、CPU调度与GPU执行。原数据保留，未把短窗口差异当精确开销。

自动任务如果无限保留，会在256条记录后达到预算。新增显式release_terminal，只允许终态且
物理完成的内部自动结果释放；外部记录不静默淘汰。测试覆盖逻辑取消但GPU仍在途时拒绝释放。
客户端禁止使用内部request_id前缀，避免撞到自动记录并在其释放后失去查询。任务ID作用域为进程，
跨重启还需配合运行实例记录，不能只用数字task_id关联整个历史。

## EV-EVIDENCE-003 · 2026-11-16 · 短窗口对照不等于正式性能验收

M5三轮各1000次和M6四组20秒均是有效原始对照，但复核施工方案13.1发现正式窗口要求至少
5分钟、关键配置重复3次。先前将其称为阶段性能完成，口径偏满。已更正README/progress和
派生summary，原始timings、配置与运行日志不变；数值/功能验收仍有效，正式性能门槛明确待补。
后续不以重复次数代替单轮持续时间，也不把几个短窗口P99平均后当作长窗口尾延迟。

## EV-VLM-002 · 2026-11-16 · 小模型的受约束选择与内存边界

自有JSONL适配器实际调用旧工程的C++ Runtime，独立R1/R2让模型生成原始JSON计划，出现重复的多个JSON对象。
严格解析拒绝了这些输出，没有执行工具；原始失败保留在板端m7-adapter-probe-r1/r2，不能通过放松解析或预置计划把失败包装成成功。
R3采用registered-tool-id-v2：模型max_tokens=1选择注册工具编号0..8，host按固定schema构造参数和版本化计划。
实际中文“查看相机状态”选择get_status，同一适配器真实图片问答仍返回棕熊；非法max_tokens=0被拒绝。
R3仅是独立模型和协议验证，不是完整自然语言任务验收。参数来自host注册schema的事实必须写入轨迹。

独立VLM两轮观测到最小MemAvailable仅98,844/94,948KiB，并发生换页。共驻检测模型的预算尚未通过，
不能承诺同时常驻或无检测影响。正在实现同一物理worker串行卸载/加载、子进程waitpid确认后切回检测；
新自有assets目录降低运行时KV/input预算的方案尚未执行，不能将配置修改当作实测节省内存。

参考donor SDK源码可见profiling容器跨请求累积，因此其tokens/s不能直接作为自有单次请求指标。
记录自有CLOCK_MONOTONIC时间，并拟每进程最多32请求回收。模型切换、取消和内存回收仍需实机验证。

本机审查还发现“get_recent_metrics返回累计status却标10秒窗口”和“异常丢失失败工具task_id”的风险。
已改为有界向前实际采样窗口并保留失败回执。Python接口模拟测试9/9通过，明确标为transport-fixture，
不计入20个真实任务或模型正确率。面试追问：为什么协议约束不能替代语义验收？逻辑取消与杀进程后的GPU释放如何确认？

### R4 · 配置字段不是可以任意缩小的运行时预算

新资产目录把max_input_len/max_kv_cache_capacity/max_kv_pool_pages改为1024/1024/8，engine和旧目录不动。
原生SDK初始化明确报Paged KV profile mismatch，past_key_values_0要求的形状与序列化engine profile不符；
6.297秒正常失败退出，尚未产生模型响应。原始run/stdout/resources保留在m7-adapter-budget-r4。
这是已证实的配置与engine契约不匹配，不是OOM，也不能靠跳过SDK校验宣称内存优化成功。
prepare_vlm_assets默认改为保持original profile；reduced-candidate必须显式选择并标为未验候选。
需要缩小固定KV profile时必须重新构建并独立回归engine。当前继续验证原profile的串行模型调度。

## EV-VLM-003 · 2026-11-16 · 串行卸载仍需核对实际分配与模型语义

M7 runtime R2实际快照和同帧检测完成，加载VLM时ROS节点被SIGKILL，未收到物理shutdown确认。
内核日志确认global OOM：21:37:39由edgevision_vlm_触发，杀死pid25598 edgevision_node。此轮失败，
不能把Action超时当作普通DDS超时。所有测试子进程已退出，未改旧edgevisiond服务或全局内存策略。

只读审计发现donor `engines/visual/action/action.engine`是指向`visual.engine`的symlink。SDK先加载
785MiB视觉engine，再尝试把这个可选文件加载为Alpamayo Action expert，第二次分配后才因模型配置
解析失败。该专家不用于本项目VQA/工具规划；新自有assets-r3不导入可选action目录，旧资产保持原样。
独立R5真实问答/工具选择/非法预算负例通过（10.293s）。这排除了不需要的瞬态分配，不代表缩小KV profile。

runtime R3的实际snapshot→同帧detect→模型plan→describe→resume_detector均返回，capture继续推进，
最终收到物理shutdown确认。但采样MemAvailable最小仍只有16,572KiB；parent/child采样RSS分别为
1,300,156/1,467,200KiB，不能相加当唯一物理占用或GPU分配。该组合没有长稳或资源预算通过结论。
正在实现仅在唯一CUDA owner销毁全部detector/event/stream且前序物理完成后，释放本进程保留CUDA
context并trim空闲allocator；候选修复尚待下一版实机验证，绝不对在途GPU工作执行reset。

该轮总体验收仍失败：Agent连续6次重复get_status，随后到达步骤预算。模型输出合法工具编号不等于
规划语义正确；不将格式正确率写成任务完成率。独立提示对照12例只做真实模型选择、不执行工具：
中文短提示4/6，英文短提示1/6，全部原响应保留；它们不计入固定22任务集成绩。下一版显式记录提示
revision，并将真实已完成工作以简明上下文传给模型，原始失败不覆盖。

测试工具首轮误用了colcon隔离安装前缀，哈希检查前即失败、节点未启动；修正为包级安装路径后另建R2。
本BSP缺少/proc/task/children，R2资源采样记录错误而未得到child指标；R3改为读取/proc/stat的PPID，
只保留自有节点直接子进程。缺失采样不补零，不用R2无效数据推断峰值。

donor processed_chat_template保留domain_lora_v1_merged来源字符串；本地base权重匹配HF缓存并不能
证明缓存engine由同一base权重导出。当前身份以实际engine/config/plugin/SDK对象哈希为准，架构为
Qwen3-VL-2B，完整engine导出谱系仍未重建，不继承旧工程性能、精度或稳定性数字。

R2候选源码实机R4复测：相同五个底层操作均返回，记录2次本进程CUDA context释放，切回后继续
完成检测，最终节点exit0且物理shutdown确认。采样MemAvailable最低158,412KiB；该短窗口与旧轮
系统状态不同，不能称为精确内存节省量或正式资源预算通过。Agent仍失败：中文短提示选择不必要的
start_capture，统一状态机拒绝STATE_CONFLICT。后续独立二元完成判断实验也只有4/6正确，存在
过早判断完成的错误，尚未接入产品，不能以它隐藏未完成步骤。修复与验收继续，22任务尚未正式运行。
