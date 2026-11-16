# M2 · 真实 USB 采集阶段记录

这是 11-15 的 M2 采集增量记录，整阶段尚未完成。11-16 新增的 30min 采集、运行中拔插与 HealthMonitor 见 [m2-runtime.md](m2-runtime.md)；新后端推理仍待接入。
主线仍是指定 jetson-inference fork，个人输入接口位于 `projects/edgevision`，
原 GStreamer 相机后端保留用于对照。

## 实现与所有权

- 从原工程 J11 采集封装迁移，精确来源见 [donor 哈希](capture-provenance.json)。
  新增帧字段、eventfd 停止、协商读回、资源预算和公共控制入口。
- 唯一 capture worker 打开设备，执行单平面 V4L2 MMAP。S_FMT/G_FMT、S_PARM/G_PARM
  读回真实值，检查 device_caps、streaming、stride、sizeimage 和实际 REQBUFS 数量。
- DQBUF 后复制到预留容量自有 slot，再立即 QBUF；GPU 不直接别名 MMAP 内存。
  默认驱动 4 buffer、自有 4 slot、READY 队列 2，按实际映射和池容量校验预算。
- ControlRouter 是 start/stop 入口。STREAMON + 有效首帧才 RUNNING，模型状态独立。
  停止唤醒 poll，停止发布、清 READY、等待已交付 lease、STREAMOFF、munmap、close，
  完成后才发送 Stopped。持有帧期间不假报物理清理完成。
- 启动 EIO/ETIMEDOUT/EOVERFLOW 可有限恢复，默认 250/500ms、最多 2 次；
  等实际清理后重开，每次增加 generation，原请求身份不变。退避期间可取消/过期/停止。
  无设备、busy 与配置错误明确失败。11-16 已新增运行中有限恢复，独立验收见 m2-runtime.md。

## 本轮实际验收

平台同 M1；YUYV 640×360，stride 1280，sizeimage 460800，实际间隔 1/30。
驱动映射及自有池各 1,843,200 字节；下列工具不执行模型、不保存图像。

| 场景 | 结果与边界 |
|---|---|
| Windows core | 5/5，含受限恢复的状态、generation、取消、超时与预算测试 |
| Jetson ASan/UBSan/LSan | 7/7；I/O 夹具 + 真实 eventfd，不以夹具冒充相机验收 |
| 首次真实相机 sanitizer 窗口 | 3s，91 帧，sequence 缺口 0，正常清理 |
| 原生正常采集 | 10s，301 帧，sequence 缺口 0 |
| 原生慢消费者 | 150ms 消费间隔，复制 301、交付 67、淘汰旧 READY 232、停止丢弃 2 |
| 原生帧池耗尽 | 持有 4 个 lease，5s 仍 DQ/QBUF；pool drop 147；释放 lease 前 stop 保持待清理 |
| 原版连续启停失败 | R2 完成 37 轮，第 38 次 S_FMT EIO，原 ledger 保留 complete=false |
| 更新后连续启停 | R3 完成 100 轮、300 帧，共 3 次受限启动重试；每轮停止后 fd=6、视频映射=0 |
| 无设备 / 设备占用 | 明确返回失败；占用测试的原 owner 保持正常采集 |

100 轮中的 RSS 停止后范围 1688–4260 KiB，不做零泄漏或长期稳定性推断。
3 次重试是真实发生的恢复，不是“100 次从未出错”。底层 UVC 控制错误根因未确认。
详见固定问题记录 **EV-CAP-002**：[debugging_journal.md](debugging_journal.md)。

## 复现入口

仅在 Linux 上显式打开 V4L2；原生资源观察使用非 sanitizer 构建。

```bash
cmake -S projects/edgevision -B projects/edgevision/build/capture-native \
  -DCMAKE_BUILD_TYPE=Release -DEDGEVISION_ENABLE_V4L2=ON \
  -DEDGEVISION_ENABLE_SANITIZERS=OFF -DBUILD_TESTING=OFF
cmake --build projects/edgevision/build/capture-native --target edgevision_capture_probe -j2

python3 projects/edgevision/tools/run_capture_acceptance.py \
  --binary projects/edgevision/build/capture-native/edgevision_capture_probe \
  --device /dev/v4l/by-id/usb-Generic_Web_Camera_20250708V1.000-video-index0 \
  --out projects/edgevision/artifacts/new-unique-run
```

输出目录必须不存在；busy 测试只针对工具自身创建的 owner。不会修改旧服务或内核。
sanitizer 构建加 `-DEDGEVISION_ENABLE_SANITIZERS=ON -DBUILD_TESTING=ON`，再执行 CTest。

## 证据与计数口径

[汇总](../evidence/2026-11-15-m2-capture/summary.json) 与原始 ZIP 保存 R1/R2/R3 实际
源码输入、构建、测试、metadata CSV、原始失败和复测。driver 时间的 clock/source
单独记录；driver→host 含 SOE 后曝光/传输，不是推理延迟。
慢消费者看到的 sequence 跳变包含主动淘汰，驱动缺口按全部 DQBUF 单独计数。

停止后的精确核对是：`dequeued = copied + pool_drop + invalid_payload`，
`enqueued = consumed + replaced + aborted`，并检查 pool、队列、fd 和视频映射。
这些检查没有覆盖 GPU 路径、长稳或所有异常。

## 下一步

新后端 30 分钟采集、运行中拔插/有限重连与 HealthMonitor 已另行记录。串行 DetectNet 输入适配
已在 [m2-inference.md](m2-inference.md) 实测，完整执行层仍待验。CUDA event 和异步执行在 M5 单独完成；ROS2/VLM/Agent/Web/RTSP
仍在完整施工范围内，不能依据本页把 M2 或后续阶段写成全部完成。
