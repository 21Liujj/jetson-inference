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

HealthMonitor 入口是 `health_monitor.h` 与 `src/health_monitor.cc`。窗口有时间与容量
双上限；跨 generation 清零，未测量返回 optional，捕获/消费/结果完成口径分开。
活跃跨模块快照不做假原子守恒；最终停止后的严格核对仍由验收工具完成。

模型 worker 的观测由同一控制 owner 调用 observe_model 发布；model_error 与
capture last_error 独立。模型失效不能让正常采集显示故障，已销毁模型不能报 READY。
真实负例和串行 GPU 输入适配见 docs/m2-inference.md；完整执行任务路由仍待接入。
