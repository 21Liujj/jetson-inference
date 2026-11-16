# M2 · 直接采集到上游 DetectNet 的输入适配

本阶段保留 `gstCamera/videoSource` 对照路径，新增可选的个人适配接口，链接指定
`build-baseline/aarch64` 下已修复的 libjetson-inference/libjetson-utils。没有新建另一套
TensorRT loader；颜色转换、归一化、执行与检测后处理均复用冻结上游。

## 数据与资源路径

```text
capture worker: DQBUF -> 自有 slot 复制 -> QBUF -> READY 队列(2)
GPU worker:    FrameLease -> 按 stride 打包到 mapped YUYV -> 上游 CUDA RGB8
               -> 上游 DetectNet -> 实际完成屏障/event -> 复制结果 -> 归还 lease
control owner: 任务/采集状态 + 独立模型状态 + 健康快照 + 停止/清理
```

首版只支持偶数宽 YUYV、最大 1920×1080；逐行去掉 padding，检查 stride、bytesused、
可用容量和目的缓冲预算。mapped 原始缓冲与 device RGB 缓冲在相邻完成的推理间复用，
不直接借用驱动 MMAP。仅一个 GPU worker 拥有模型、context 和这些缓冲。

冻结上游 `Detect` 调用 `ProcessNetwork(true)`，其 ONNX 分支使用同步 executeV2。
此正确性基线保持 default stream，额外 `cudaDeviceSynchronize` 后记录、等待真实 event，
才把 INFLIGHT lease 转为 DONE。同步是有意保守的设计；**没有异步重叠或性能优化结论**。
若完成屏障返回不可恢复错误，代码按非零状态隔离整个工作进程，不让析构提前归还仍
可能被 GPU 访问的内存。该故障分支尚未注入实测。

控制线程不调用 Detect，也不析构在途 lease。停止先阻止接收新推理，提交公共 capture
stop，持续 poll；capture worker 等 lease 释放后才完成物理清理。模型状态在 control
owner 发布，模型加载失败可以同时显示 capture=RUNNING、model=ERROR。正常销毁模型
后改为 UNLOADED，不留下 READY 的失实快照。

## 验收边界

- R6 首轮：模型就绪后 10s，302 次完成/发布；本次画面全部为有效空检测结果，
  不据此证明检测精度。总进程 20.5463s，包含初始化；579 次复制、277 次 READY 淘汰，
  pool drop/driver sequence 缺口为0，停止后 pool_busy=0，正常退出。
- R7 负例：故意给不存在的模型路径，返回 MODEL_FILE_NOT_FOUND、exit1；日志同时观察
  capture=RUNNING/model=ERROR，随后 capture=IDLE、无占用，不伪装成有效空检测。
- 新的行布局与模型独立状态测试：Windows 8/8；Jetson ASan/UBSan/LSan 10/10。
  sanitizer 构建不加载 CUDA 模型，其结果不代表 CUDA 内存检查。
- R7：60s、1,802 次推理，使用 fallback=false 的新 engine；该配置差异保留。
- R8：对齐 M1 fallback=true，实际加载日志与 SHA256 确认复用 M1 engine；60s 窗口
  完成/发布1,802次，总进程63.1163s。复制1,864帧、READY淘汰62，pool drop与driver
  sequence缺口为0；最终IDLE/UNLOADED、pool_busy=0、视频无owner，正常退出。
  两轮画面都只有有效空结果，不是检测精度证明。

完整执行层的 submit/query/collect/cancel_pending/shutdown、公共任务路由、GPU 运行中
超时/取消的真实测试仍未接通，**M2 仍为部分完成**。M3 数值/精度回归、M5 异步流水线
和正式性能验收保持独立；不能把基础 event 的使用写成已优化。

## 构建与复现

默认 host-only 构建不探测 CUDA。Jetson 上显式开启：

```bash
cmake -S projects/edgevision -B projects/edgevision/build/inference-native \
  -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF \
  -DEDGEVISION_ENABLE_V4L2=ON -DEDGEVISION_ENABLE_UPSTREAM_INFERENCE=ON \
  -DEDGEVISION_UPSTREAM_PREFIX="$PWD/build-baseline/aarch64"
cmake --build projects/edgevision/build/inference-native --target edgevision_inference_probe -j2
projects/edgevision/build/inference-native/edgevision_inference_probe \
  --device /dev/v4l/by-id/usb-Generic_Web_Camera_20250708V1.000-video-index0 \
  --model projects/edgevision/artifacts/m1-model/ssd-mobilenet.onnx \
  --labels projects/edgevision/artifacts/m1-model/labels.txt \
  --seconds 60 --out projects/edgevision/artifacts/new-inference-run
```

模型仍为固定 batch1、FP32 I/O、FP16 compute 的 SSD 水果基线。
模型与库的实际哈希、构建、逐帧结果 metadata、诊断 CSV 和原始错误随证据包归档。
不保存相机图像；窗口分位数只是诊断，非正式性能 benchmark。上游 YUYV 颜色转换的
矩阵/量程尚未与设备色度元数据做完整核对，M3/M5另行验证。

[最终R8汇总](../evidence/2026-11-16-inference-adapter/summary.json) ·
[逐文件哈希](../evidence/2026-11-16-inference-adapter/manifest.json) ·
[原始R6/R7记录](../evidence/2026-11-16-inference-adapter/raw-inference-adapter.zip) ·
[R8对齐复测](../evidence/2026-11-16-inference-adapter/raw-inference-aligned-r8.zip)。
配置差异与workspace记录纠正在固定 [journal EV-GPU-002](debugging_journal.md)。
