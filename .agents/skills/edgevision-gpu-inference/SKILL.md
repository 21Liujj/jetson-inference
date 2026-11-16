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

M2 个人输入适配入口 `src/inference/upstream_detector.cc` 和
`tools/capture_inference_probe.cpp`，显式 CMake 开关及独立 upstream prefix 见
docs/m2-inference.md。保持 default stream：冻结上游同步 ONNX executeV2 不采用
传入的非阻塞 stream，不能直接改 stream 就宣称顺序正确。该基线使用粗粒度实际完成
屏障与 event 保留 lease；不是 M5 异步执行器。模型错误单独发布，不覆盖采集状态。
读取本轮证据时分清 R6 初版与 R7 收尾，不把空检测当模型未运行或检测精度已验。

最终输入基线为 R8（fallback=true），加载 M1 同一 engine；R6/R7 的 fallback=false
缓存是另一份 engine，已保留原始结果。比较时核对实际加载日志、参数与 engine SHA，
不能仅凭 ONNX SHA 或缓存文件存在。固定 journal EV-GPU-002 记录了这次配置漂移。
模型 manifest 的 logged_workspace_request_bytes 是日志请求，TRT10 上游没有显式
setMemoryPoolLimit；不要重新把该数字写成已生效的内存上限。

公共物理执行器在 src/inference/upstream_executor.cc，控制接口见execution_backend.h；
不能另建逻辑任务历史。默认一个GPU worker/context/job/completion，消费自有lease。
取消/过期只失效结果，物理完成后才发布completion。R10真实GPU与联合拔插验收见
docs/m2-executor.md。gpu_acceptance_delay.cu是显式测试注入，默认关闭；不能当CUDA优化。
该验收target显式使用Shared runtime和SM87，nvcc走绝对路径，不改变全局环境。

M3 YOLO分支已验：models/yolov8n-coco.json是首轮前冻结契约，
docs/m3-model.md及evidence/2026-11-16-yolo记录200张子集AP、24图逐字节CPU预处理、
真实视频与YOLO/SSD任务复测。DetectorKind::SsdFruit仍是默认；YoloV8Coco显式选择，
后者继承修复后的tensorNet，没有独立TRT loader。旧export仅metadata可审计，不冒称重导出。
新engine为2925e839…0474519，FP16 builder flag不表示所有算子均半精度。
R4测试错误假定重启只增一代；R5严格核对1+start_retries，生产恢复策略未改变。

当前M5起点仍是串行GPU颜色转换→CPU letterbox→GPU tensor，见upstream_detector.cc。
P0诊断profile在板端artifacts/2026-11-16/m5-p0-diagnostic-r1，nsys2024.5.4已实测可采集；
它不是锁频正式benchmark。先冻结新数值门槛，再实现融合和P0/P1/P2相同输入对照，
不能因为M3用了CUDA event就声称M5已经完成。框阈值翻转真实样例见journal EV-GPU-003。
