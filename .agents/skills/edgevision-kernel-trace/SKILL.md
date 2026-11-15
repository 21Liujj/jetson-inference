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
