# EdgeVision · Jetson 视觉与多模态 Agent 系统

基于 [jetson-inference](https://github.com/dusty-nv/jetson-inference) 二次开发，面向 USB 相机采集、UVC 追踪、CUDA/TensorRT 执行、ROS2 与本地视觉任务 Agent。

**当前：M2–M4 基础验收完成；M5 数值/功能及五配置各三轮5分钟吞吐/延迟对照通过，资源对照待补；M6 ROS2 功能已通过、正式性能待补；M7 集成开发中，M8 待验收。**
未修改上游在 TensorRT 10 下的崩溃已保留；修复版结果与原版分别记录。

[展示页](https://21Liujj.github.io/jetson-inference/) · [实机基线](projects/edgevision/docs/baseline.md) · [进度](projects/edgevision/docs/progress.md) · [贡献边界](UPSTREAM.md) · [上游原版说明](README.upstream.md)

## 已有结果

平台：Jetson Orin Nano 8GB · Ubuntu 22.04.5 · L4T 36.4.3 · CUDA 12.6 · TensorRT 10.3。

| 验证项 | 本轮实际结果 |
|---|---|
| 上游构建 | 指定提交的 detectnet、video-viewer 板端原生编译成功 |
| 工具采集 | YUYV 640×360，1,800 帧，窗口内 sequence 缺口 0 |
| 驱动帧间隔 | 29.9934fps；MONOTONIC / SOE 时间戳 |
| TensorRT 兼容修复 | 修复 context 初始化、命名 I/O 绑定、显式 batch 维度及辅助输出索引 |
| 实际推理 | 固定图输出检测框；最终版 60 秒窗口完成 1,712 次相机推理 |
| 30 分钟基线 | 完成 53,898 次真实相机推理，正常退出，日志逐文件哈希核验 |
| M2 核心 | 有界队列、帧租约、任务与控制路由；Windows 9/9，Jetson sanitizer 11/11 |
| YOLOv8n 数值基线 | 200张子集FP16 AP下降0.047个百分点；固定图框匹配99.5%，24张CPU预处理逐字节相同 |
| YOLO真实视频 / 任务 | 60秒完成1,803次；YOLO和原SSD各7项GPU生命周期复测通过；最新CPU测试10/10、板端sanitizer12/12 |
| UVC源码扩展 | 新增帧完成/内部回收与stream追踪；172导入CRC匹配，原版/关闭/开启三组实测与实际回退通过 |
| 内核与用户态关联 | 慢消费241帧=消费80+READY替换159+停止2；真实拔插7.38s恢复，1,803条消费全部关联 |
| CUDA融合与数值回归 | 48例预处理通过；200图AP下降0.065个百分点，固定框匹配99.5% |
| GPU离线长窗口对照 | 五配置各3×300秒、646,305次完成；Pinned中位：P0 74.09 → P1 129.72 → P2 192.68 images/s；对应P99 14.751 / 11.549 / 14.377ms |
| 异步与真实完成 | Nsight证实跨stream GPU区间重叠；融合相机1802次推理、7项GPU生命周期测试通过 |
| ROS2 实机闭环 | 图像/检测/诊断、统一服务与Action；GPU在途取消/超时、慢订阅、受限bag与回放通过 |
| 直接 V4L2 采集 | MMAP、真实协商、eventfd 停止；慢消费者与帧池耗尽分别计数 |
| 100 次真实启停 | 复测完成，共 3 次受限启动重试；每轮停止后 fd 恢复基线、视频映射归零 |
| 新后端 30 分钟 | 53,999 帧，无序号缺口或应用丢弃；60s 后该窗口 RSS / fd 持平 |
| 运行中拔插 | 5 次/30s 预算下，4 次重开后恢复；检测故障到有效首帧 12.38s |
| 新后端推理 | 接入上游 DetectNet；复用 M1 engine，60 秒完成 1,802 次实际推理 |
| 统一执行层 | 检测与启停共用 TaskManager；真实 GPU 超时、取消、停止与 shutdown 通过 |
| 联合拔插 | GPU 在途帧保留到完成；3 次重开后新会话继续检测，恢复首帧 7.57s（5 次/30s预算） |
| 停止优先级 | 断连时显式停止，插回后进程继续观察 5s，保持 IDLE、没有重开 |
| 退出与重启 | 正常退出后再次启动完成 203 次推理；错误 tensor 名明确失败 |
| GPU 回归 | 完整输出及辅助输出两种场景，真实 enqueueV3、形状与有限值检查通过 |

以上为分版本验收。M5长窗口使用固定RGB、同engine与NMS、每轮排除60次warmup，逐帧核验计数和分位数；[原始记录与哈希](projects/edgevision/evidence/2026-11-16-gpu-formal/manifest.json)保留每轮和分钟窗口。P1/P2边界、资源采样缺项及profile扰动见[GPU报告](projects/edgevision/docs/m5-gpu-pipeline.md)，ROS闭环与四组短窗口开销见[ROS2报告](projects/edgevision/docs/m6-ros2.md)。M5资源对照和ROS正式长窗口仍待补。相机为30fps，离线吞吐不代表摄像头帧率。最终系统30分钟/2小时/12小时长稳与不可恢复GPU破坏性故障注入尚无通过结论。

[原始日志与哈希清单](projects/edgevision/evidence/2026-11-15/manifest.json) · [完整文本证据包](projects/edgevision/evidence/2026-11-15/raw-text-evidence.zip) · [故障定位](projects/edgevision/docs/debugging_journal.md)

[M2 采集与失败复测记录](projects/edgevision/docs/m2-capture.md) · [长采集、恢复与指标](projects/edgevision/docs/m2-runtime.md) · [上游推理输入适配](projects/edgevision/docs/m2-inference.md) · [统一执行与 M2 验收](projects/edgevision/docs/m2-executor.md) · [Agent 开发入口与领域技能](AGENTS.md)

## 系统范围

| 方向 | 目标 | 状态 |
|---|---|---|
| 采集 | 单 owner V4L2、MMAP、有界帧池、启停与断连恢复 | M2 基础验收完成；分版本证据可追溯 |
| 内核 | 匹配 L4T 的 UVC 帧完成 trace，关联用户态记录 | M4 实机验收完成，系统原模块已恢复 |
| GPU | 数值回归、缓冲复用、stream/event、Nsight 对照 | 数值/功能和3×5分钟吞吐/延迟通过；资源对照待补 |
| 机器人 | 图像、检测、诊断、服务与 Action | M6功能通过；正式发布开销验收待补 |
| 多模态 | 本地 VLM、计划校验、真实工具与报告 | Qwen3-VL独立问答通过；M7工具闭环开发中 |
| 交付 | Web/RTSP、日志限制、部署恢复与长期运行 | M8 待验收 |

ROS2 与 Agent 是正式交付项。静态展示页用于查看工程证据，实时设备控制台另行验收。Orin Nano 的编码能力按实测处理，不宣称 NVENC 硬编码。

Agent 面向自然语言拍照、固定类别检测、图像问答、指标查询与报告。当前实机底层工具链已串通，自动规划仍会选择错误或重复工具；完整任务验收尚未通过，见 [M7 实现与边界](projects/edgevision/docs/m7-agent.md)。

## 最小运行

在匹配 CUDA/TensorRT 的 Jetson 上执行。保持原版子模块 SHA；关闭会自动安装依赖和 PyTorch 的上游预构建脚本。

~~~bash
git clone --branch edgevision/v3.1 https://github.com/21Liujj/jetson-inference.git
cd jetson-inference
git submodule update --init utils ros c/plugins/pose tools/camera-capture
cmake -S . -B build-baseline \
  -DBUILD_DEPS=OFF -DBUILD_INTERACTIVE=OFF -DCMAKE_BUILD_TYPE=Release
cmake --build build-baseline --target detectnet video-viewer -j2

python3 projects/edgevision/tools/fetch_baseline_model.py \
  --out projects/edgevision/artifacts/m1-model
build-baseline/aarch64/bin/detectnet data/images/banana_0.jpg /tmp/result.jpg \
  --model=projects/edgevision/artifacts/m1-model/ssd-mobilenet.onnx \
  --labels=projects/edgevision/artifacts/m1-model/labels.txt \
  --input_blob=input_0 --output_cvg=scores --output_bbox=boxes --headless
~~~

[开发依赖、相机运行与 GPU 回归命令](projects/edgevision/docs/baseline.md) · [模型来源与 SHA256](projects/edgevision/models/ssd-fruit-baseline.json)

## 代码与记录

~~~text
c/tensorNet.cpp                     TensorRT 10 兼容性修复
projects/edgevision/
  include/ src/ tests/              C++ 帧池、租约、队列、任务核心与回归
  tools/                           环境采集、监督运行、实机回归、证据导出
  models/                          模型契约与哈希；权重/engine 不入库
  docs/                            审计、基线、调试记录、施工规范与验收表
  evidence/2026-11-15/              本轮原始文本证据与逐文件哈希
  site/dist/                       简洁工具面板风格的静态展示页
~~~

保留 [EdgeVisionRT](https://github.com/21Liujj/EdgeVisionRT) 与 [jetson-stm32F407](https://github.com/21Liujj/jetson-stm32F407) 作为既有工程。旧工作区成果先审计再复用，旧里程碑不自动等同于本轮验收。

## 贡献与许可

上游主仓库固定为 45da40a8，utils 固定为 6d5471c9，ROS 固定为 9b5a668b。上游相机、CUDA 算子和 TensorRT 框架归为复用；本轮个人修复、测试和工程记录逐项列入 [UPSTREAM.md](UPSTREAM.md)。

保留 NVIDIA 上游 [MIT 许可证](LICENSE.md) 与版权声明。
UVC补丁及原内核文件保留[对应GPL许可](projects/edgevision/kernel/README.md)。
模型metadata与权重来源限制见[部署基线](projects/edgevision/docs/m3-model.md)。

