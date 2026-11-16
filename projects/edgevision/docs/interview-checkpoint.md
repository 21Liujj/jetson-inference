# 面试检查点 · 核心视觉链路已验证，Agent集成未完成

以[验收表](progress.md)为准，保留各组件版本与原始失败。M0–M4基础验收已完成；
M5数值/功能和长窗口吞吐、延迟对照通过，资源对照待补；M6功能通过，正式性能待补。
M7底层工具链已通，自动规划不稳定；M8实时交付与最终长稳待做。

## 当前可采用的简历表述

项目名称可用“基于Jetson的机器人视觉系统（V4L2 / CUDA / ROS2）”；完整设计仍包含多模态Agent。

- 基于jetson-inference二次开发，新增C++ V4L2/MMAP采集后端、有界帧池与帧租约，统一管理启停、
  任务超时/取消和USB断连恢复；完成100次启停、30分钟采集及真实拔插验证。
- 扩展UVC帧与stream追踪，关联内核事件和用户态sequence/session记录，用于区分驱动交付、
  应用主动丢帧和消费积压；完成原版/追踪关闭/追踪开启对照及实际模块回退。
- 部署YOLOv8n TensorRT模型，改造CUDA融合预处理、缓冲复用和stream/event执行；完成200图
  数值回归与Nsight Systems分析，集成ROS2图像、检测、诊断、Service及Action并实机联调。

需要一个性能数字时，可补：“固定RGB离线对照，每配置3×5分钟，Pinned路径吞吐中位数
74.09→129.72 images/s；各轮P99的中位数14.751→11.549ms。”输入是离线RGB，不能写成相机帧率。

多模态进展可口头说明“已接入本地VLM，验证同帧问答与检测模型串行切换；Agent规划仍在调试”。
暂不写“已完成自然语言拍照、检测、问答、报告全流程及长期稳定运行”。

## 面试最值得准备的追问

### 1. MMAP buffer、自有帧池和FrameLease分别解决什么？

驱动MMAP缓冲需要及时QBUF；下游不长期占用它。有效数据复制到固定大小自有池，READY队列
有界，慢消费者可以替换待消费旧帧，但不能抢占GPU在途slot。lease把资源归还与实际生命周期
绑定。区分sequence缺口、READY替换和pool耗尽，不能把它们全部叫摄像头丢帧。
对应[m2-capture](m2-capture.md)、[m2-core](m2-core.md)。

### 2. 超时或取消后，为什么不能马上释放GPU输入？

逻辑任务可以先进入EXPIRED/CANCELED，但GPU可能还在读取同一内存。保留物理pending、frame
lease和slot，直到实际完成事件/屏障确认，再允许复用。测试通过真实GPU延迟注入观察这段窗口，
而不是只sleep模拟GPU。停止/shutdown也等待这条边界。对应[m2-executor](m2-executor.md)。

### 3. 内核trace如何与用户态帧对应？

核对设备身份、kernel stream、sequence及单调时钟；断连重开还需映射用户态generation，不能
只按sequence或用户frame_id拼接。区分交付、内部回收和取消事件，检查trace丢失后再关联。
本项目有1,803条实际消费记录关联；没有宣称修复UVC底层丢帧缺陷。
对应[m4-uvc-trace](m4-uvc-trace.md)。

### 4. CUDA优化的收益和正确性如何证明？

RGB letterbox、归一化及布局转换融合，固定缓冲复用，stream/event表达依赖。先验证48例
预处理和同200图模型门槛，再做相同输入/engine/NMS的性能对照。Nsight证明部分跨stream
GPU区间重叠；它不自动证明缓存命中率或带宽。YUYV颜色转换仍复用上游，不能算自己新kernel。
对应[m5-gpu-pipeline](m5-gpu-pipeline.md)。

### 5. P2吞吐更高，为何运行时仍选P1？

P2是离线两slot组件，吞吐提高时P99也变差；相机本身约30fps，离线190+ images/s不是摄像头
190fps。吞吐、排队、结果帧龄和资源预算要一起看。ROS时间戳是发布时间，采集为MONOTONIC，
时钟域未确认不能直接相减。对应[m5报告](m5-gpu-pipeline.md)、[ROS2报告](m6-ros2.md)。

### 6. 工程中最实际的两类失败是什么？

ROS构建成功而安装运行崩溃：实际加载了系统旧动态库。通过GDB、RUNPATH和运行时路径校验
定位，并保留原始失败。M7则暴露8GB内存压力、无用可选模型分配及错误工具选择：合法输出
不等于语义正确，未通过的任务不能填完成率。问题正文在固定[journal](debugging_journal.md)，
重点看EV-ROS-002、EV-VLM-002/003。

## 仍需明确的边界

当前没有多相机/IMU同步、硬件触发/PTP、OTA或NVENC硬编码验收。静态展示页不是实时设备控制台。
组件30分钟通过不代表最终系统30分钟/2小时/12小时通过。仓库基于上游二次开发，个人新增与
复用边界见[UPSTREAM](../../../UPSTREAM.md)；源码快照、engine和实际加载库哈希比板端HEAD更可靠。

后续恢复顺序：修复M7规划与资源预算 → 固定22任务/三条轨迹/取消竞争 → 补M5/M6资源与正式性能
→ M8实时Web/RTSP、部署恢复和最终长稳。RK3588项目另按用户新要求启动，不混用两块板的验收结果。
