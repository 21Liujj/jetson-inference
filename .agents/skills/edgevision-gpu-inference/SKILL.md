---
name: edgevision-gpu-inference
description: 修改 jetson-inference 的 TensorRT 加载、检测输入适配、CUDA 预处理、stream 或 event 流水线时使用。
---

# GPU 推理

入口为 `c/tensorNet.cpp`、`c/detectNet.cpp`、`c/detectNet.cu`，按实际任务读取。
模型契约见 `projects/edgevision/models/ssd-fruit-baseline.json`；兼容性回归工具是
`projects/edgevision/tools/trt_compat_probe.cpp`。

- 先查 [baseline](../../../projects/edgevision/docs/baseline.md) 与固定问题记录。
  原版 TRT10 SIGSEGV 与修复版分开；context 初始化、命名 tensor 地址、batch 和
  辅助输出索引是本轮兼容修复，不是 CUDA 性能优化。
- 目前已验路径是固定 batch 1、FP32 I/O、FP16 compute 的 SSD 水果基线。
  动态 shape、其他 dtype、最终模型和精度均要另验。
- 上游转换/归一化和 DetectNet 保留贡献归属。先接稳定输入基线，再做自己的融合
  预处理、缓冲复用、stream/event；不可用一个新推理框架替代指定二次开发主线。
- launch 成功不是完成。真实 event 完成前保留输入输出、lease 和 context；
  超时只使结果失效。不可恢复 CUDA 错误走 worker 失败边界，不提前复用内存。
- 数值回归冻结输入、预处理、阈值、输出布局和误差口径。性能结论需要 warmup、
  完成时间、固定功耗/热状态、重复测量与 Nsight 时间线；一次窗口计数不能替代。

大权重和 engine 不入 Git。准确性、吞吐、P95/P99、执行重叠和访存结论各自链接证据；
源码编译或同步探针通过不代表异步流水线已经验收。
