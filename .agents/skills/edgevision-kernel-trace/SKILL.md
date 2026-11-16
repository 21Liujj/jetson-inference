---
name: edgevision-kernel-trace
description: 准备匹配 Jetson L4T 的内核源码、分析 UVC 调用链、开发帧级 trace 或验证模块回退时使用。
---

# L4T 与 UVC

当前冻结环境、模块和历史日志从 [baseline](../../../projects/edgevision/docs/baseline.md)
及 progress 查找；按需读施工方案第 7 节。先检查实际 uname、release、config、
modinfo、vermagic、Module.symvers 与设备是否仍一致，不能把通用 Linux 5.15 源码
当作已核验的 NVIDIA 目标源码。

- 先构建并验证匹配的未修改模块/内核，再做小范围源码补丁。
  已有 V4L2/vb2 trace 与 dynamic debug 是覆盖分析的起点，不是新增驱动功能。
- 新 trace 要关联 UVC 帧完成、sequence、buffer 状态、generation 和用户态记录；
  标明驱动时间戳的 clock/source，不混淆 CUDA kernel 与 Linux kernel。
- 安装前准备原模块、匹配依赖和可执行回退路径；切换时处理真实视频使用者。
  不强制加载不匹配模块，不因一次 EIO 就编造固件或驱动缺陷结论。
- 分别保存原版、修改但关闭 trace、启用 trace 的记录；比较正常、慢消费者、
  启停和实际拔插，最后实际验证回退。

无匹配源码、构建配置或回退证据时，把对应门槛记为未完成，继续可独立的分析。
问题只在固定 debugging_journal 维护正文，相关内核报告链接该条记录。

M3匹配源码/原版构建/实际模块切换回退已验，见docs/m3-kernel.md及
evidence/2026-11-16-kernel-baseline。fetch_kernel_sources.py只冻结源码，
build_uvc_module.py只构建原版，verify_uvc_baseline.py分prepare/verify。
实际加载模块读取sysfs GNU Build ID，不以默认modinfo磁盘文件替代。
未签名模块在现有sig_enforce=N加载后taint12288，回退不清除此标志，不擅自重启。

M4入口kernel/0001-uvc-frame-observability.patch与uvc_trace.h，
make_uvc_trace_patch.py可从冻结13文件复现补丁；build_uvc_trace.py只构建。
uvc_trace_capture.py使用独立tracefs instance，不清理全局trace。
verify_uvc_trace.py分prepare/matrix/recovery/restore，实验路径均finally回退。
parse_uvc_trace.py按物理USB路径、内核stream_id、sequence、单调时刻关联消费CSV，
内核stream_id与用户generation显式映射；歧义或trace丢失不能判通过。
M4两轮矩阵与真实拔插/回退已验，见docs/m4-uvc-trace.md、
evidence/2026-11-16-uvc-trace。R1矩阵并行engine构建；R2仅为DVFS未锁定的开销观测，
不能宣称零开销。真实reason=1内部回收已出现，不与READY替换/取消归还混算。
用户generation1→4映射内核stream1→2；不能要求二者数值相同。运行模块现为系统原版。
