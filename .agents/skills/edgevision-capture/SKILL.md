---
name: edgevision-capture
description: 开发或排查 Jetson USB 相机的 V4L2 MMAP、格式协商、采集线程、停止和重连时使用。
---

# USB 采集

先读 `projects/edgevision/src/camera/` 中本次涉及的 v4l2_capture 或 v4l2_backend，
以及对应头文件。设备验收读 [采集记录](../../../projects/edgevision/docs/m2-capture.md)；
协议要求按需读施工方案第 6 节，不全量加载规范。

- 设备 owner 是唯一 capture worker。先核对 `/dev/v4l/by-id` 或 by-path、节点能力
  与占用者；显式授权的不稳定路径才允许 videoN。不要复用旧服务或额外打开预览相机。
- 按驱动实际 width/height/fourcc/stride/sizeimage、REQBUFS 数量和映射长度工作；
  预算包含驱动映射与自有帧池。仅支持单平面 capture，未支持的格式明确失败。
- DQBUF → 校验 → 复制到预留容量 slot → 立即 QBUF。无空槽仍需 DQ/QBUF 并计数。
  driver sequence 缺口、坏 payload、pool drop、READY 淘汰、停止丢弃分别统计。
- poll 同时监听相机与 eventfd。停止通知不能靠另一线程 close 正在使用的 fd。
  停止发布、清队列、等消费者 lease、STREAMOFF、munmap、close 完成后才发 Stopped。
- 启动瞬态错误只能按明确分类有限重试，先清理再重开并增加 generation。
  busy、非法配置和无设备不可被无限重试掩盖；运行中拔插恢复需单独实测。

Linux 构建加 `-DEDGEVISION_ENABLE_V4L2=ON`，主机默认 OFF 不探测硬件依赖。
用 `capture_probe.cpp` 和 `run_capture_acceptance.py` 保存真实 metadata、命令和错误。
fixture、真实短采集、100 次启停、30 分钟及拔插验收分别记账，不能互相替代。

运行中恢复由 ControlRouter 的会话意图驱动：Stopped 后退避重开、换 generation、
有效首帧后 RUNNING。预算跨一次显式 start 累计，成功不重置；stop 清除会话意图。
仅稳定设备路径自动恢复。真实观测和参数见 docs/m2-runtime.md；R3 长采集与
R4/R5 恢复源码不同，不互相套用。人工测试先确认 ready，再观察进程存活期间的事件；
没有覆盖断连或插回的窗口必须判未通过。症状与处理统一记 journal EV-CAP-003/EV-TEST-001。
