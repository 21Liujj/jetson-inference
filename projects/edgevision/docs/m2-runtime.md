# M2 · 运行中恢复与健康指标

2026-11-16，真实 Orin Nano / L4T 36.4.3。M2 仍未整体验收：上游 DetectNet
输入适配与实际执行层尚待接入。这里记录纯采集，不含推理、CUDA 优化或内核补丁。

## 已验证行为

| 场景 | 版本与结果 |
|---|---|
| 直接后端 60s / 30min | R3：1,800 / 53,999 帧；30min 监督时长 1801.0877s，正常退出；无 sequence 缺口、pool drop、READY 淘汰或启动重试 |
| 30min 资源观察 | 3,596 次采样；60s 后 RSS 4,272 KiB、fd 7；停止后 fd 6、视频映射 0。仅限该窗口 |
| Windows CPU 核心 | R4：7/7；恢复与 HealthMonitor 新增独立测试 |
| Jetson sanitizer | R4：ASan/UBSan/LSan 9/9，包含显式 syscall 夹具，不冒充真实拔插 |
| 真实运行中拔插 | R4：gen1 故障，4 次有限重开，gen5 收到有效帧并继续交付；检测故障到有效首帧 12.3811s，正常退出 |
| 恢复中显式停止 | R5：gen1 断连后 stop；相机节点重新出现时进程仍驻留，继续 5s 保持 IDLE、generation 不变、无 worker/设备资源；正常退出 |
| 原始无效操作窗口 | R4 stop 测试在 180s 内没观察到断连，返回 RECOVERY_NOT_OBSERVED；保留失败，未计入通过 |

R3 长采集、R4 核心/自动恢复、R5 观测工具分别冻结源码，不能把 R3 的 30min
结论套在修改后的程序上。R5 只修改 probe：人工等待上限改为 600s，并等待实际重新
枚举后持续观察；核心保持 R4。运行中拔插测试显式配置 **5 次 / 30s** 恢复预算，
不代表默认 3 次 / 15s 已通过同一拔插场景。

## 控制与所有权

运行中错误先停止发布，等待 lease、STREAMOFF、解除映射和 close 的实际完成。
控制 owner 收到 Stopped 才进入有限退避并调度下一次打开；worker 不自行无限重连。
每次重开增加 generation，必须再次收到有效首帧才 RUNNING。已完成 start 请求的历史
结果保持原 execution_generation，不改写成自动恢复后的成功。

重开总次数属于一次显式 start，恢复成功不补充预算；另有单次恢复 deadline。
仅稳定 by-id/by-path 允许自动重开。stop 在故障清理、退避或等待首帧时都清除持续运行
意图；逻辑任务结束不能代替物理清理。旧 generation 帧和事件不发布为当前结果。

## 指标口径

HealthMonitor 的捕获率来自有效 DQBUF 累计数，包含无空槽而主动丢弃的有效帧。
固定容量、时间窗口、样本数与跨度一起输出；host dequeue→消费和→结果完成分别采样。
没有结果样本时结果分位数为空，不填 0。它们不是 CUDA event 耗时、正式 P99 benchmark
或 driver SOE→用户态交付延迟。切代清空窗口，拒绝旧帧结果；资源值未采到时保持未知。

## 复现与证据

```bash
cmake --build projects/edgevision/build/capture-native --target edgevision_recovery_probe -j2
projects/edgevision/build/capture-native/edgevision_recovery_probe \
  --device /dev/v4l/by-id/usb-Generic_Web_Camera_20250708V1.000-video-index0 \
  --seconds 600 --out projects/edgevision/artifacts/new-recovery-run
# 等待 ready_for_disconnect 再操作；另一个唯一输出目录加 --stop-during-recovery。
```

[R3 长采集汇总](../evidence/2026-11-16-capture-30min/summary.json) ·
[R4/R5 汇总](../evidence/2026-11-16-runtime-recovery/summary.json) ·
[逐文件清单](../evidence/2026-11-16-runtime-recovery/manifest.json) ·
[原始日志与实际源码包](../evidence/2026-11-16-runtime-recovery/raw-runtime-recovery.zip)。
源码包 SHA256、构建、测试、事件、健康 CSV、监督退出和原始失败均归档；未保存私有图像。
问题与面试追问统一见 [固定 journal](debugging_journal.md) 的 EV-CAP-003、EV-METRIC-001、EV-TEST-001。
