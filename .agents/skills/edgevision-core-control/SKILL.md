---
name: edgevision-core-control
description: 修改 EdgeVision 帧池租约、有界队列、TaskManager 或 ControlRouter 的所有权和状态契约时使用。
---

# 帧与控制核心

源码路径均相对仓库根。按实际修改选读 `projects/edgevision/include/edgevision/`
中的 frame、frame_lease、frame_queue、bounded_queue、task_manager、control_router，
再读对应 `src/` 和 `tests/`，无需加载相机、ROS 或模型代码。

- WRITING 由采集 worker 拥有；队列只放 READY。淘汰旧 READY 不得回收 INFLIGHT。
  lease 的 epoch 防槽位复用，capture generation 防旧会话结果，两者不能混用。
- 逻辑 CANCELED/EXPIRED 不等于物理完成。实际执行结束前保留执行槽与内存；
  CompletionFence 的 CPU 夹具不证明 CUDA event 已实现。
- 控制状态由一个 poll owner 改写。后端调用在 router 锁外；取消与首帧发布通过
  同一锁排序。accepted、首帧 RUNNING、任务完成分别验证。
- 重开由控制层增加 generation，原请求身份保持不变。恢复次数、退避、deadline
  和取消都有限；禁止在 backend 内偷偷重开但沿用旧 generation。
- TaskManager 默认 1 个排队、1 个执行、256 条保留记录，不静默淘汰幂等历史。

验证入口：core-only CMake 与 `ctest`；所有权、状态和并发场景见
[核心契约](../../../projects/edgevision/docs/m2-core.md)。涉及设备时再读 capture skill。
真实问题追加到固定 debugging_journal，验证边界更新 progress，避免按测试目录名判断平台。
