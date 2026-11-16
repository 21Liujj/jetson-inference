# M2 · 统一任务与真实 GPU 生命周期验收

2026-11-16，M2 基础验收完成。各门槛按实际源码版本保留证据：R3 采集100次启停/
30min，R8 新输入60s，R10 公共执行器、物理超时/取消、停止和联合拔插。
这不是最终系统的长稳结论；M3–M8 保持完整施工范围。

## 一个任务管理器，两种完成

CLI、后续 ROS2/Agent/Web 都通过 ControlRouter。StartCapture、StopCapture 与
DetectSnapshot 共用同一个 TaskManager、task_id、request_id 幂等记录和状态查询。
相机与模型状态独立；其他运行时命令仍按后续阶段接入，不把接口预留算成功能完成。

| 接口 | 语义 |
|---|---|
| initialize | 非阻塞请求 GPU worker 加载冻结模型；查询 LOADING/READY/ERROR |
| submit | 统一校验/受理任务，返回 accepted、task_id；不等于已经完成 |
| query | 公共逻辑任务状态；执行器另提供只读物理阶段快照 |
| collect | 逻辑终态且 execution_pending=false 才交付记录；重复读取不重复执行 |
| cancel_pending | 取消尚未派发的公共等待任务；已提交 GPU 不被假装中断 |
| shutdown | 停止受理、撤销等待项、请求相机停止与 GPU 排空，最后确认物理退出 |

GPU worker 独占上游模型/context、mapped/raw/RGB缓冲和在途FrameLease。
执行器只有一个物理job和一个completion槽，不另建任务历史。GPU实际完成前，
EXPIRED/CANCELED记录仍execution_pending=true；迟到成功不能把原终态改成DONE。
backend调用均在router锁外，控制线程不等待CUDA或析构在途lease。

默认一个排队/一个执行/256条保留记录；达到预算明确拒绝，不静默淘汰幂等历史。
有效stop可以替换排队检测，并优先停止采集发布；它仍需等待当前GPU和capture清理。
无效generation、过期或冲突的stop不能撤销其他任务。停止后的新清理错误会使stop失败。

结果JSON最多16KiB，包含task/generation/frame/sequence与实际检测；最多包含64个框，
总数及truncated显式返回。空数组是有效无目标结果，加载失败有独立错误。
当前检测模型仍为M1固定SSD水果模型，尚无M3准确率结论。

## 实际结果

| 门槛 | 证据 |
|---|---|
| Windows core | R10 9/9；旧8项和新的统一执行路由均通过 |
| Jetson ownership regression | ASan/UBSan/LSan 11/11；夹具测试不加载GPU、不冒充相机 |
| 真实任务生命周期 | 7项：普通/幂等、排队取消、在途超时、在途取消、停止抢占、换代重启、shutdown |
| 模型失败独立 | 缺失模型时capture=RUNNING/model=ERROR，检测FAILED且无结果；正常排空退出 |
| 在途证明 | 过期前先见RUNNING及CUDA event not-ready；终态时仍not-ready，slot为INFLIGHT；完成后才清physical pending |
| 停止顺序 | STOPPING时READY队列0、唯一在途pool slot仍为1；之后pool0、视频映射0，再次启动到新generation |
| 联合真实拔插 | 断连时真实GPU event仍pending；旧任务失败不发布结果，等待其完成再重开；3次重开至gen4，新会话2个检测DONE |
| 联合恢复时间 | 故障检测到有效首帧7.5720s；本次使用5次/30s预算，不等于默认3次/15s结论 |
| 联合运行结束 | 776次实际GPU提交/完成、1个故障任务无结果；最后IDLE/UNLOADED、pool0/maps0、正常退出 |

上述pending窗口使用**显式CUDA测试负载**，200ms是请求值，不是性能测量。
它让真实设备工作保持未完成，防止测试在launch之前就取消而误判。该kernel不是预处理
优化，产品默认构建不开启。联合测试用4096条有界历史容纳人工操作窗口，不是M8长期
服务保留策略。最大poll时间仅为本轮观测值，不是实时性保证。

## 复现

保持指定上游库/engine，显式启用验收工具；不升级板端CUDA/TensorRT。

```bash
cmake -S projects/edgevision -B projects/edgevision/build/inference-native \
  -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF \
  -DEDGEVISION_ENABLE_V4L2=ON -DEDGEVISION_ENABLE_UPSTREAM_INFERENCE=ON \
  -DEDGEVISION_BUILD_GPU_ACCEPTANCE=ON \
  -DCMAKE_CUDA_COMPILER=/usr/local/cuda/bin/nvcc -DCMAKE_CUDA_ARCHITECTURES=87 \
  -DEDGEVISION_UPSTREAM_PREFIX="$PWD/build-baseline/aarch64"
cmake --build projects/edgevision/build/inference-native \
  --target edgevision_executor_probe edgevision_executor_recovery_probe -j2
projects/edgevision/build/inference-native/edgevision_executor_probe \
  --device /dev/v4l/by-id/usb-Generic_Web_Camera_20250708V1.000-video-index0 \
  --model projects/edgevision/artifacts/m1-model/ssd-mobilenet.onnx \
  --labels projects/edgevision/artifacts/m1-model/labels.txt \
  --out projects/edgevision/artifacts/new-executor-run
```

联合拔插用recovery工具与新的唯一输出目录，等ready_for_disconnect后操作。
所有工具仅保存metadata，不保存私有图像。不可恢复CUDA失败的进程隔离分支未做
破坏性注入；最终整机故障恢复与2h/12h仍由M8验收。

[汇总](../evidence/2026-11-16-executor/summary.json) ·
[逐文件哈希](../evidence/2026-11-16-executor/manifest.json) ·
[实际源码、命令、构建、测试与原始事件](../evidence/2026-11-16-executor/raw-executor-evidence.zip)。
工程问题唯一正文见 [journal EV-CTRL-002](debugging_journal.md)，包含原始错误测试、
nvcc PATH问题与测试runtime链接配置的处理，未覆盖/失败记录不删除。
