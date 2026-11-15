# 基于Jetson的机器人视觉与多模态Agent系统

版本 V3.1　更新日期 2026年11月15日

项目主线为 USB 摄像头采集、Linux UVC 驱动扩展、CUDA与TensorRT端侧部署、ROS2集成和视觉任务Agent。本文用于个人项目实施及阶段验收；所有性能数值均为设计目标或实验参数，完成板端测量后填写实测结果。

本版确定以jetson-inference及配套submodule作为开源基础，依据AI芯片系统软件实习岗位开展二次开发。先运行并冻结上游基线，再增加真实设备管理、任务状态、日志和部署能力，把C++资源管理、GPU异步执行和内核源码修改形成可核查成果。ROS2和Agent均为正式交付项。Jetson与RK3588分别完成独立可演示的系统，STM32不再是两项目的前置依赖。

## 1 项目目标与本版调整

最终系统接收USB相机图像，在Jetson上持续执行目标检测，发布图像、检测结果与健康状态，并接受CLI、ROS2和Agent的统一控制。用户可以输入“开始检测并保存当前画面的结果”“拍一张照片并描述画面”“查询最近一分钟为什么丢帧”。系统返回实际工具结果、关联图像和任务记录。

项目名称建议为“基于Jetson的机器人视觉与多模态Agent系统”。简历中按实际完成情况突出UVC驱动扩展、C++推理执行层、CUDA优化、ROS2和Agent；完成一项再增加一项。

| 交付方向 | 本版必须完成的内容 | 可核查的成果 |
|---|---|---|
| USB采集 | C++ V4L2 MMAP、有界帧池、启停和重连 | 源码、30分钟采集记录、异常恢复记录 |
| 驱动开发 | 匹配目标L4T的UVC源码构建与可观测性扩展 | 内核补丁、构建记录、模块信息、真实trace |
| 端侧部署 | 固定小模型的TensorRT C++部署及正确性验证 | ONNX与engine清单、预测结果、精度对照 |
| GPU执行层 | 缓冲区复用、异步提交、stream与event管理 | 资源所有权设计、压力验证、时间线 |
| ROS2 | 图像与检测发布、诊断、服务与长任务Action | ROS包、launch、CLI验证、rosbag样例 |
| Agent | 自然语言规划、受约束工具调用、结果校验 | 模型适配器、任务轨迹、真实端到端示例 |
| 工程化 | 日志指标、故障诊断、systemd和长稳 | 部署步骤、故障报告、2小时及12小时记录 |

### 1.1 二次开发的贡献边界

每项成果分别标记复用、修改或新增。上游已经具备的相机输入、CUDA算子、TensorRT加载与ROS节点，先保留并验证。自己的工作集中在实际缺口、可测瓶颈和完整系统行为，开发记录同时给出上游版本与个人修改。

| 方向 | 上游已有基础 | 本项目修改或新增 |
|---|---|---|
| 相机 | jetson-utils的GStreamer与V4L2输入 | 统一状态、稳定设备选择、帧统计；按需增加直接V4L2后端 |
| GPU图像处理 | 颜色转换、resize、归一化与映射内存工具 | 数值回归、实际热点优化、生命周期与多帧执行管理 |
| TensorRT | 模型加载、执行与视觉示例 | 模型适配、任务队列、查询与过期结果处理 |
| ROS2 | ros_deep_learning图像和检测节点 | 状态诊断、服务、Action及统一控制入口 |
| Agent | 已有本地模型及NanoLLM相关参考 | 真实工具适配、计划校验、任务轨迹与资源预算 |
| 内核 | 匹配L4T的Linux UVC驱动 | 可观测性扩展或实际问题修复，单独保存内核补丁 |

ROS2、Agent、网页状态与RTSP网络预览为正式系统交付。STM32不作为前置依赖；Docker、开放词汇分割及Jetson额外外设按剩余时间扩展。

## 2 开源基础与版本冻结

### 2.1 主项目与配套仓库

主项目确定为dusty-nv/jetson-inference。它提供Jetson上的C++与Python视觉推理示例，配套jetson-utils负责相机与CUDA图像处理，ros_deep_learning提供ROS与ROS2节点。后两者已经纳入主仓库submodule，首轮沿用主仓库记录的配套提交。[R14][R15][R16]

| 仓库 | Star数 | 本项目用途 |
|---|---|---|
| dusty-nv/jetson-inference | 9,033 | 主fork、TensorRT执行与端到端视觉基线 |
| dusty-nv/jetson-utils | 914 | 相机、视频、CUDA图像处理与内存工具 |
| dusty-nv/ros_deep_learning | 985 | ROS2图像与检测基线、节点适配参考 |
| dusty-nv/NanoLLM | 381 | 多模态Agent与插件结构参考 |
| NVIDIA-AI-IOT/ros2_nanollm | 198 | ROS2与视觉语言模型接口参考 |

查询日期为2026年11月15日。主项目默认分支当前提交为45da40a8f3c180191b269f57f736caaa025b8a69，提交日期2025年10月16日；utils的配套提交为6d5471c97dad443776d2339569a4d1145f034a9e。实施时记录实际检出的完整SHA；这些查询记录不代表已完成板端兼容性验证。

NanoLLM与ros2_nanollm的查询时最近推送分别在2024年10月与2024年8月。Agent优先复用自己已能运行的模型接口，参考其插件与ROS连接方式，单独验证模型、依赖和内存，避免把旧示例的环境直接覆盖到当前软件栈。

### 2.2 先原样验证再改代码

基线A是未修改上游：固定图像、兼容ONNX模型、USB输入、相同模型的实时执行。基线B是加入自己的状态和日志但保持算法一致的版本。只有两者记录完整，后续吞吐、延迟、稳定性和恢复行为的改进才有明确参照。

源码中可以确认TensorRT 10条件分支及enqueueV3路径，但仍需实际编译、模型加载和数值验证。历史issue记录过L4T 36.4.3预构建镜像缺失，因此先核验可用镜像或按当前环境构建；不把README中的JetPack 6支持等同于所有模型和所有容器tag都已适配。[R17][R18]

本项目本轮完成文档与公开源码核查，未在用户Jetson上运行。板端测试结果由开发阶段实际填写。

### 2.3 Fork与源码管理

已有个人fork时沿用；没有远端条件时先在本地建立开发分支，不让远端操作阻塞本地审计。保留上游remote、版权与许可证。新增UPSTREAM.md，记录仓库、SHA、submodule SHA、上游原始行为、个人修改与验证证据。

上游目录保留原结构。建议在主fork新增projects/edgevision作为个人系统层，按需要增加最小CMake入口。修改submodule时在相应个人fork或分支中提交，再更新主仓库gitlink；只提交主仓库不能保存未提交的submodule文件修改。

| 已核对的源码位置 | 阅读与改造用途 |
|---|---|
| c/tensorNet.cpp | engine加载、输入输出绑定、stream与执行路径 |
| examples/detectnet | 上游检测应用与输入输出闭环 |
| utils/camera/gstCamera.cpp | 主仓库固定版本的相机输入实现 |
| utils/video/videoSource.cpp | 新采集后端接入与统一视频接口 |
| utils/cuda | 已有CUDA预处理与内存工具审计 |
| ros/src/node_detectnet.cpp | ROS2检测节点与消息转换参考 |
| projects/edgevision | 本项目新增状态、任务、指标与工具接口 |
| kernel/patches | 相对目标L4T源码的独立UVC补丁与验证说明 |

路径以主仓库固定的submodule版本为准，独立仓库最新分支可能已有目录调整。不得因README路径变化而同时拉取不匹配的依赖版本。

## 3 今晚的开工顺序

今晚以约4小时为参考，目标是原样运行开源基础并留下基线。先确认上游在当前设备能够构建和执行，再进入自己的采集后端、任务框架与内核改造。ROS2和Agent保留在正式阶段中。

| 时间预算 | 实际动作 | 交付记录 |
|---|---|---|
| 前30分钟 | 环境、仓库、设备与依赖审计 | env、repo-audit、相机格式清单 |
| 30至90分钟 | 获取并冻结主仓库与submodule，构建最小目标 | UPSTREAM、SHA、构建输出 |
| 90至150分钟 | 运行上游兼容ONNX模型的固定图推理 | 模型hash、原始预测与时间记录 |
| 150至210分钟 | 用工具与上游视频入口验证USB采集和推理 | 实际格式、帧统计、错误与退出记录 |
| 最后30分钟 | 核对基线、列个人修改清单 | baseline报告、贡献矩阵与下一阶段 |

编译、模型下载或板端依赖超出预算时保留原始错误，并继续源码审计与独立可做的工作。M0/M1不会因为时间到达就自动通过，也不以模拟结果代替实机运行。

### 3.1 源码与环境检查

下面命令在个人项目位置执行。已有工作目录或fork时复用并先检查未提交改动，不覆盖重克隆。首次获取示例为：

```bash
git clone --recursive https://github.com/dusty-nv/jetson-inference.git
cd jetson-inference
git rev-parse HEAD
git submodule status --recursive
git status --short
```

根据已记录的SHA固定一次可运行基线。构建步骤以对应commit的README、CMake和当前依赖为准，先检查安装脚本，再选择当前阶段需要的目标，避免提前下载或构建所有训练组件。

```bash
task_out="artifacts/env/$(date +%Y%m%d_%H%M%S)"
mkdir -p "$task_out"
uname -a > "$task_out/uname.txt"
cat /etc/nv_tegra_release > "$task_out/l4t.txt"
dpkg-query -W 'nvidia-l4t-*' 'libnvinfer*' 'cuda-cudart-*' \
  > "$task_out/packages.txt" 2>&1
command -v nvcc v4l2-ctl trtexec nsys colcon ros2 \
  > "$task_out/tools.txt" 2>&1
free -h > "$task_out/memory.txt"
lsusb -t > "$task_out/usb_topology.txt"
v4l2-ctl --list-devices > "$task_out/video_devices.txt" 2>&1
modinfo uvcvideo > "$task_out/uvc_module.txt" 2>&1
```

### 3.2 相机基线与今晚完成条件

先枚举真实capture节点，再选择其支持的格式。示例640×480 YUYV 30fps必须出现在枚举结果中；不支持时换成实际组合。metadata节点与视频节点分开。

```bash
camera_dev=/dev/video0
v4l2-ctl -d "$camera_dev" --all
v4l2-ctl -d "$camera_dev" --list-formats-ext
v4l2-ctl -d "$camera_dev" \
  --set-fmt-video=width=640,height=480,pixelformat=YUYV \
  --set-parm=30 --stream-mmap=4 --stream-count=300 \
  --stream-to=/dev/null --verbose
```

之后依据已构建上游程序的help接入同一个设备，验证连续输出与正常退出；推理使用已独立通过的模型。保存实际输入、模型、日志、进程资源和观察结果，先运行60秒，再逐级扩大到30分钟。

今晚完成M0表示版本、依赖、源码和相机清单清楚；M1完成表示上游固定图与USB路径真实运行，有可复现命令和原始输出。此时才进入个人改造，不把启动成功算成后续驱动、CUDA优化或Agent已完成。

环境预检不升级JetPack，不替换内核。后续内核模块切换基于匹配源码、备份和回退步骤开展。

## 4 岗位要求与两个项目的分工

该岗位覆盖AI芯片driver、firmware和用户态runtime，并要求C/C++、Linux kernel、CUDA机制及软硬件调试。项目选题据此分配如下。覆盖程度表示项目可提供的实践证据，不表示已具备相应岗位熟练度。

| 岗位要求 | Jetson的实施内容 | RK3588的补充内容 |
|---|---|---|
| C/C++与资源管理 | V4L2、RAII、帧池、GPU任务生命周期 | 设备采集、RKNN适配、应用调度 |
| Linux kernel | UVC与videobuf2缓冲区、映射、唤醒、回收 | I2C或SPI client、IIO、regmap、设备树 |
| CUDA软件层级 | CUDA Runtime API、自定义kernel、TensorRT执行 | 通过RKNN用户态接口理解NPU部署分层 |
| 异步launch、stream、event | 多帧流水线、事件依赖、异步完成查询 | RKNN能力允许时开展异步及缓冲复用实验 |
| DMA与内存 | 区分USB DMA、CPU复制、CUDA传输和共享内存 | 外设数据路径、DMA与中断按实际硬件验证 |
| ARM与软硬件调试 | USB带宽、错误日志、模块版本、温度和频率 | 原理图、pinmux、供电、总线与中断联调 |
| 机器人系统 | ROS2视觉接口、诊断、Agent任务入口 | ROS2外设数据和RKNN结果适配 |

Jetson项目开发的是基于CUDA与TensorRT的应用侧执行层，同时扩展真实UVC内核驱动。RK3588项目需要实际编写或扩展外设驱动。调用CUDA、TensorRT或RKNN分别归入用户态部署与执行能力；GPU或NPU底层驱动开发应有对应内核代码和硬件执行证据。

优先深入的内核方向为“视频缓冲区的内存映射与生命周期”，配套掌握VMA、页引用、等待队列、锁、workqueue和DMA地址的基本区别。项目结束时应能解释这些机制在实际调用链中的作用，并用一个补丁和一次故障分析支撑。

## 5 环境基线与系统架构

### 5.1 目标环境

历史设备记录为Jetson Orin Nano Super 8GB、JetPack 6.2、L4T 36.4.3、Ubuntu 22.04、CUDA 12.6系列。本轮未连接板端检测；以今晚采集的版本清单为实施依据。TensorRT、编译器、内核配置、UVC模块形态和相机型号均需确认。[R1][R2]

先沿用板端已安装且匹配的CUDA与TensorRT。ROS2默认评估Ubuntu 22.04对应的Humble，已安装其他适配版本时记录差异再决定。Agent复用现有EdgeQwen的前提是该仓库在当前设备完成独立推理验收；历史开发进度不作为本项目测试结果。

### 5.2 模块与执行边界

| 层次 | 模块 | 职责与边界 |
|---|---|---|
| 真实输入 | USB相机与Linux UVC驱动 | USB传输、帧组装、V4L2缓冲交付 |
| 采集核心 | V4L2Capture与FramePool | 协商格式、采集、时间戳、所有权、重连 |
| GPU执行 | BufferPool、Executor、TensorRTBackend | 预处理、异步提交、完成查询、输出解析 |
| 运行管理 | ControlRouter、HealthMonitor | 唯一启停入口、状态机、预算、指标与错误 |
| 机器人接口 | edgevision_ros | 图像和结果发布、服务、Action、诊断 |
| 任务编排 | VisionAgent与ToolRegistry | 解释自然语言、调用工具、校验真实结果 |
| 展示部署 | CLI、轻量Web、systemd | 可观测控制与运行管理 |

个人系统层首版链接上游libjetson-inference与libjetson-utils，采用单个C++进程加有界工作线程，避免在数据路径未清楚前引入跨进程帧生命周期。ROS2适配器可与核心同进程运行，Python Agent通过版本化本地控制接口访问核心；采集fd只由一个采集线程拥有。

需要GPU故障隔离时再拆成capture_daemon与inference_worker。拆分后通过共享内存的偏移量、slot和generation传递CPU帧，命令使用已有POSIX MQ或版本化Unix socket；像素不放进MQ。CUDA内存跨进程共享另做能力检查，不能直接传递裸指针。

### 5.3 统一控制与状态

CLI、ROS2、Agent和Web全部调用ControlRouter。命令至少携带request_id、session_generation、command、deadline_ms和参数，返回accepted及task_id；最终结果由状态查询或任务事件给出。重复request_id返回已有任务，不重复执行。

采集状态为IDLE、STARTING、RUNNING、STOPPING、RECOVERING和ERROR。RUNNING要求STREAMON成功且收到有效首帧；模型状态另列UNLOADED、LOADING、READY、BUSY和ERROR。相机正常但模型失败时显示采集正常与推理失败两个事实。

stop顺序为停止接收新任务、停止发布新帧、清空未执行队列、等待在途GPU任务、回收自有帧、STREAMOFF、解除映射并关闭设备。等待超时进入明确失败状态，仍被GPU使用的内存不能提前复用。

### 5.4 ARM64、BSP与系统调试

Jetson项目也保留ARM64/Linux底层实践：匹配L4T的源码、config与模块构建，UVC补丁安装与回退，用户态/内核态边界、MMAP与DMA地址、等待唤醒及锁。补AArch64整数传参反汇编、一次系统调用与驱动入口关联、一次相机故障调用链分析，不将GPU CUDA kernel与Linux内核混为同一层。

Jetson保留完整的摄像头、驱动、执行层、模型、ROS2、Agent、网络展示与部署系统；RK3588另承担Boot、全类外设、PHY、MPP、HMI、语音与RTOS的全面终端范围。两边有重叠模块，通过深入问题和硬件后端区分。

## 6 USB采集与C++基础实现

### 6.1 设备与格式协商

先完成上游gstCamera基线。直接V4L2后端作为一个独立可选输入实现接到videoSource或个人适配接口，保留原GStreamer后端用于行为对照；设备仍只有一个owner。

使用O_NONBLOCK与O_CLOEXEC打开设备，检查QUERYCAP中的真实采集和streaming能力。若V4L2_CAP_DEVICE_CAPS置位，依据device_caps判断节点能力。首版明确支持单平面capture；遇到仅多平面设备返回UNSUPPORTED，之后单独实现。

实现S_FMT与读回、帧率请求与读回、REQBUFS、QUERYBUF、mmap、QBUF、STREAMON、poll和DQBUF。按驱动返回的width、height、pixelformat、bytesperline、sizeimage、bytesused工作，不使用固定宽高直接推算所有缓冲容量。[R3]

poll同时监听采集fd与停止通知eventfd。EINTR重试，EAGAIN返回等待，真实错误进入恢复流程。不能让无帧时的忙轮询占满一个CPU核心。SIGTERM只触发退出通知，复杂清理在正常线程上下文完成。

### 6.2 帧结构与所有权

FrameMeta至少包含frame_id、session_generation、V4L2 sequence、width、height、fourcc、stride、bytesused、buffer_flags、driver_timestamp及其类型、host_dequeue_mono_ns、publish_mono_ns。序号统计考虑uint32回绕，重启stream后另起generation。

第一条稳定路径为：DQBUF后校验数据，复制到自有原始帧slot，完成CPU复制后立即QBUF；推理消费自有slot。建议驱动buffer从4个开始，自有slot从4个开始，待处理队列容量2，均在启动时校验总内存预算。

自有slot状态为FREE、WRITING、READY、INFLIGHT、DONE。FrameLease为move-only资源；队列过载只能回收READY状态的旧帧。INFLIGHT关联GPU完成事件，事件完成后才能归还。旧generation的结果被标记过期并丢弃。

后续若直接让GPU读取某个采集buffer，必须延后该buffer的QBUF直到GPU读取完成，并评估驱动buffer被占用导致的采集背压。任何“少一次复制”的优化都需重新检查所有权与新鲜度。

### 6.3 采集验收

| 场景 | 应有行为 | 需要保存的证据 |
|---|---|---|
| 正常采集30分钟 | 帧统计持续推进，资源有界 | CSV、帧间隔分布、设备配置 |
| 连续启停100次 | 可重新打开，fd与映射不持续增加 | 每轮结果、fd与RSS趋势 |
| 设备不存在或被占用 | 返回明确错误，未报告RUNNING | 错误码、状态事件 |
| 非法分辨率或格式 | 拒绝或记录最终协商结果 | 请求值与实际值 |
| 拔插相机 | 状态退化，按稳定设备标识有限重连 | 内核日志、恢复时间、generation |
| 推理变慢 | 丢弃待处理旧帧，采集保持可观测 | 队列深度、丢弃原因、帧龄 |

优先使用/dev/v4l/by-id或可确认的物理路径选择设备。没有稳定唯一标识时要求显式配置，不能默认为拔插后的video0仍是同一摄像头。

## 7 Linux UVC驱动开发主线

### 7.1 真实开发目标

核心驱动成果为“UVC帧完成与缓冲区状态的可观测性扩展”。先用内核已有tracepoint和dynamic debug检查现有覆盖，再针对缺少的信息增加一个小而完整的UVC tracepoint。补丁应能回答一次实际采集异常中的具体问题，例如帧是否已经在驱动完成、何时交给用户态、错误buffer与用户态丢弃分别有多少。

候选源码位置包括drivers/media/usb/uvc中的uvc_driver.c、uvc_video.c、uvc_queue.c，以及videobuf2核心和对应allocator。实际文件与函数以目标L4T源码为准。先查include/trace/events中已有的V4L2和vb2事件，避免重复实现已有能力。[R2][R4]

### 7.2 实施步骤

1. 保存uname、内核配置、modinfo、模块路径、vermagic和原始相机基线，确认uvcvideo为模块还是内建。
2. 获取与当前L4T release对应的源码和构建配置，先构建未修改的模块或内核，并验证它与现有基线行为一致。headers不足以替代完整驱动源码和匹配的Module.symvers。
3. 跟踪open、REQBUFS、mmap、QBUF、STREAMON、USB完成回调、帧完成、DQBUF、STREAMOFF与disconnect调用链，画出buffer状态及锁保护关系。
4. 用已有事件完成初始trace；确定UVC特有缺口后，在合适的帧完成或归还位置新增tracepoint及配套解析脚本。
5. 编译并保存补丁、配置差异、构建日志与模块信息；在板端测试原版与修改版，再记录启用和关闭trace时的开销。
6. 覆盖正常流、慢消费者、启停、拔插及实际支持的异常场景。人工注入场景与观察到的真实缺陷分别记录。

### 7.3 补丁的最小功能规格

建议字段包括设备或stream标识、buffer sequence、有效字节数、完成状态和内核单调时间戳。若需要队列深度和stream generation，应先确定其维护位置、并发保护及新增开销。不能为填字段读取没有生命周期保障的对象。

正常情况下每帧一个事件，不在每个USB包或URB中打印日志。频繁路径避免printk；昂贵参数仅在事件启用时计算。USB完成上下文中的代码遵守既有上下文限制，不新增可能睡眠的调用。拔插与stop路径必须检查对象引用、锁序和回调完成。

同时提交用户态trace解析工具，将内核事件与采集CSV按设备、stream generation和sequence关联。时间戳需确认同一时钟域或建立有记录的换算。展示一张真实时间线，解释驱动完成、用户态出队、排队和GPU处理之间的等待。

### 7.4 内核构建与回退要求

构建前确认目标release、配置、编译器、符号版本和模块依赖。运行时切换模块前停止所有视频使用者，保留原模块与恢复命令；模块内建时采用对应内核构建和可回退启动项。模块签名、Secure Boot、厂商依赖或版本不匹配按实际报错处理，不强制加载不匹配模块。

脚本将环境收集、编译、安装和回退分成独立入口。安装前显示目标路径和备份结果，安装后验证实际加载的模块。计划中的操作不写成已执行记录，构建成功也不等于板端验证成功。

### 7.5 驱动阶段验收

必须有可应用的patch、匹配源码版本、成功构建记录、实际运行模块信息、原始trace、解析结果和回退记录。新增功能应在至少一个真实场景中提供原先缺失的诊断信息。若已有事件足够，先完成源码级分析，再选择有明确需求的UVC扩展，不能只把启用现有日志写成驱动功能开发。

增强方向为实际相机支持的UVC扩展控制、时间戳或错误恢复问题修复。开展前先确认描述符、协议或真实缺陷；没有观察到bug时，主成果保持功能扩展，不编造“修复驱动缺陷”。

## 8 TensorRT端侧模型部署

### 8.1 固定一个小模型建立基线

先运行上游已兼容的ONNX模型，再以YOLOv8n作为模型适配任务；固定输入shape、batch为1，实际模型来源和导出工具版本在manifest中冻结。已有可用小模型时可以沿用，先说明输入布局、类别、输出结构和后处理，再决定是否替换。[R7]

实施顺序为参考框架预测、ONNX导出与验证、目标Jetson构建engine、固定图片对照、视频流接入、性能测量。PyTorch或ONNX参考可以在开发机生成，保存固定输入与原始输出；Jetson执行TensorRT并逐项比对。模型导出不要求与TensorRT运行在同一台机器，engine构建默认在目标兼容环境完成。

manifest至少记录模型和ONNX的SHA256、代码版本、权重来源、导出参数、opset、输入shape与dtype、颜色空间、归一化、letterbox规则、类别表、NMS阈值、TensorRT与CUDA版本、GPU型号、构建精度、workspace设置和engine的SHA256。

```bash
/usr/src/tensorrt/bin/trtexec \
  --onnx=models/detector.onnx \
  --saveEngine=models/detector_fp16.engine \
  --fp16 --skipInference
```

这是TensorRT 10系列的示例；先定位本机trtexec并读取其help。需要指定动态shape、workspace或其他参数时按实际版本补充。缺少权重、ONNX或工具时报告阻塞项，不写空engine占位文件。[R6]

### 8.2 C++后端职责

TensorRTBackend负责加载manifest、检查输入输出契约、反序列化engine、创建execution context、预分配tensor buffer、设置tensor地址、enqueue和输出解析。TensorRT 10系列优先依据实际头文件使用命名tensor与enqueueV3，不照搬旧版binding接口。

一个并发worker独占自己的execution context、stream和在途buffer。首版只用一个推理worker；跨推理并发需要独立context，评估增加的activation和buffer内存后再启用。Logger与其他依赖对象的生命周期覆盖其使用者。[R6]

### 8.3 正确性验收

先用至少20张固定图像覆盖不同尺寸、空画面、密集目标和边界目标，核对颜色顺序、resize、padding、tensor布局、输出解析、NMS及坐标回映。再冻结一个带标注的小验证集，建议至少200张，比较相同样本上参考框架与TensorRT的mAP及失败样例。

FP16的精度回退阈值在首轮评估前登记；初始可把mAP绝对下降不超过0.5个百分点设为检查目标，超过后先定位来源，不直接调整阈值以掩盖差异。该目标需要结合模型和任务确认。INT8作为部署增强项，另设代表性校准集与独立验证集，记录精度、延迟和内存。

模型没有检测到目标时返回有效空结果；engine加载失败或CUDA错误返回明确失败状态，两者在ROS2和Agent中保持区别。

## 9 CUDA与C++推理执行层

### 9.1 执行层的接口

对外提供initialize、submit、query、collect、cancel_pending和shutdown。submit返回task_id与提交状态，query区分QUEUED、RUNNING、DONE、FAILED、EXPIRED和CANCELED。enqueue返回只表示提交过程推进，GPU实际完成以event或同步结果为依据。

资源通过RAII管理，队列使用有界mutex与condition_variable先建立正确实现。单独定义CudaStream、CudaEvent、DeviceBuffer、PinnedBuffer和FrameLease的生命周期，不引入与当前需求无关的通用调度框架。

超时优先取消未执行任务；已经launch的GPU工作逻辑上可标记过期，但输入输出buffer保留到真实完成。cudaEventQuery返回not ready时继续按预算查询。出现不可恢复CUDA错误时进入worker失败流程，不能归还仍在使用的slot并继续运行。

### 9.2 自定义预处理kernel

先审计上游已存在的颜色转换、resize、归一化与内存工具，记录现有调用链。CUDA改造针对实际缺口或profile热点，可以增加模型特定letterbox与布局转换，或在现有kernel基础上做有数值回归的融合优化。先完成单步正确实现，再合并能减少中间buffer的步骤。支持哪种输入格式在配置中明确：原始YUYV、NV12或解码后的RGB分别有自己的布局。

输入校验覆盖stride、padding、有效长度、奇数尺寸处理和边界像素。颜色矩阵、量程和插值规则需与CPU参考一致。保存固定输入、CPU输出、CUDA输出以及误差分布；阈值按dtype和算法在比较前确定。之后再接模型精度回归，防止数值小差异通过阈值与NMS放大。

优化依据包括内存访问连续性、每线程工作量、block布局、launch数量和中间数据写回。至少解释一个真实热点及一次前后对照，不以“用了CUDA”代替优化结论。

### 9.3 内存方案实验

Jetson的CPU与集成GPU共享物理DRAM，不同分配方式仍有访问、缓存和同步差异。实验记录实际API和数据路径，不能照搬独显经PCIe传输的假设。[R5]

| 方案 | 数据路径 | 要验证的内容 |
|---|---|---|
| A稳定基线 | 采集MMAP复制至自有host buffer，再到device buffer | 正确性、CPU复制与传输开销 |
| B固定pinned池 | 采集复制至pinned staging，异步传输后计算 | API行为、CPU开销和实际重叠 |
| C mapped host | 在支持条件下由GPU读取映射host buffer | 访问模式、缓存影响、是否更快 |
| D Unified Memory | 在支持条件下由CPU/GPU按阶段访问managed buffer | 同步、缓存管理与访问限制 |
| E DMA-BUF互操作 | 仅在采集驱动与GPU导入路径均支持时开展 | buffer类型、格式兼容与同步 |

启动时查询并保存host register、mapped host、managed memory、concurrent managed access、async engine等能力。不同buffer类型需要分别验证；支持cudaHostRegister不表示任意驱动mmap都能注册。固定预分配池是默认实现，不把cudaMallocAsync支持作为前置条件。

V4L2 MMAP描述内核与用户态的映射；DMA-BUF描述设备或驱动之间的buffer共享；CUDA异步传输描述GPU相关执行接口。三者分别标注，Linux DMA API中的设备总线地址、CPU物理地址和用户态指针也要区分。[R3][R8][R9]

### 9.4 异步流水线的迭代

版本P0为单帧串行，P1为固定buffer复用，P2为双或三缓冲与事件依赖，P3按实测决定是否增加并发推理context。每个版本保持相同模型、输入和后处理，分别测吞吐与尾延迟。

推荐从“单个推理context加预处理流水线”起步：预处理完成后记录event，推理stream等待相应event；推理结束后记录完成event，消费完成后回收slot。不同帧可以尝试重叠，同一帧的依赖必须完整。context被重新绑定或再次并发使用前保证其调用和资源状态符合后端约束。

stream间操作可能重叠，收益取决于硬件资源、工作量与依赖。用Nsight Systems观察时间线，确认是否真正重叠；吞吐提高而P99变差时，报告排队与资源竞争的代价。CUDA Graphs仅在launch开销成为真实热点且接口支持时作为增强实验。[R5][R6][R12]

### 9.5 性能证据

NVTX标注capture、copy、preprocess、enqueue、inference、postprocess、publish和agent阶段。正式性能先在不带profiler时测量；profile只采集代表性短窗口，并记录额外开销。保存原始nsys报告、统计表、优化前后配置与对应git commit。

GPU阶段耗时用放在相应stream中的CUDA event测量；排队、CPU提交与端到端耗时用单调时钟测量。不同统计口径分别命名，不把enqueueV3的CPU耗时当作GPU推理完成时间。

## 10 ROS2正式集成

### 10.1 包与节点设计

在Ubuntu 22.04的目标环境优先采用ROS2 Humble及rclcpp，构建使用ament与colcon。核心C++库不依赖ROS2；edgevision_ros包装核心能力，edgevision_interfaces保存自定义消息、服务和Action，edgevision_bringup保存launch与参数。[R10]

首版ROS节点与采集推理核心同进程，读取已经完成且具有所有权保障的快照与结果。只有一个模块打开相机。主推理链路直接使用内部帧池，ROS图像发布作为独立限频分支，初值5Hz；之后需要全频图像时另测DDS序列化和带宽成本。

### 10.2 Topic与控制接口

| 接口 | 类型或建议定义 | 首版约定 |
|---|---|---|
| /edgevision/image | sensor_msgs/Image | 发布rgb8预览，width、height、step与数据一致 |
| /edgevision/detections | vision_msgs/Detection2DArray | 目标类别、置信度、框；另保存frame_id关联 |
| /edgevision/frame_meta | 自定义FrameMeta | frame_id、generation、原始时钟域与单调时间 |
| /diagnostics | diagnostic_msgs/DiagnosticArray | 相机、模型、队列、资源和最近错误 |
| /edgevision/set_capture | 自定义SetCapture服务 | 返回accepted与task_id，最终状态单独查询 |
| /edgevision/get_status | 自定义GetStatus服务 | 返回实际采集、模型和运行状态 |
| /edgevision/run_inspection | 自定义RunInspection Action | 处理拍照检测或视觉问答，支持feedback与取消请求 |

消息字段依据安装的vision_msgs版本实现，类别ID与类别名称的映射进入manifest。自定义接口明确版本与单位，ROS消息序列化交给ROS2实现，不沿用裸C结构体的内存布局。

图像默认SensorDataQoS、best effort、小深度；诊断与控制使用相应可靠通信设置。验证发布订阅端的QoS兼容性，慢订阅者不能无限占用核心帧池。QoS配置是可测参数，不把可靠传输等同于端到端实时性。[R11]

### 10.3 时钟与线程约束

header.stamp属于ROS时间域，不能把steady_clock或V4L2单调时间戳的数值直接填入。实时采集默认use_sim_time为false；若建立capture到ROS时间的映射，记录换算方式与误差。尚未建立时用发布时刻并显式标记stamp_source，延迟仍按单调时间单独测量。

frame_id使用camera_optical_frame等已定义坐标名。相机内参未标定时保留未标定状态，不编造CameraInfo或外参，也不把二维检测框解释为真实距离。

使用独立callback group管理控制、诊断和数据发布；耗时推理与模型加载进入核心worker，不堵塞ROS executor。Action取消先撤销排队任务，在途GPU任务按资源生命周期完成清理。

### 10.4 ROS2验收

必须通过ros2 topic list、topic info与受控频率检查；用真实图像和检测结果完成一次可视化；用ROS服务完成启停并观察实际状态；用Action完成成功、失败和取消场景；录制一段受限大小的rosbag2并复现订阅侧行为。

比较ROS关闭、只发诊断、低频图像和全频图像四种配置的CPU、内存、吞吐与P95。ROS进程或订阅者退出后，按照选定进程边界验证影响范围。进程内优化和共享内存DDS能力分别验证，不能仅凭启用intra-process就宣称整条链路零拷贝。

## 11 视觉任务Agent正式开发

### 11.1 Agent的用途与完成条件

Agent接收自然语言请求，生成受约束的执行计划，调用真实工具，检查任务结果后给出带依据的回答。首版做视觉任务和系统诊断，直接使用项目已有能力。必须至少完成一个真实模型参与理解或规划的闭环；规则命令和预置计划用于开发验证时明确标为deterministic模式。

模型后端优先审计并复用已有EdgeQwen或其他已能在Jetson独立运行的小型视觉语言模型。适配器屏蔽具体推理方式，可接现有C++ Runtime或本地推理服务。模型版本、输入图片、提示模板和结构化输出解析规则进入版本清单；本阶段不要求新训练模型。

### 11.2 工具注册与真实执行

| 工具 | 输入示例 | 返回值与约束 |
|---|---|---|
| get_status | 时间窗口 | 实际设备、模型、任务、队列和最近错误 |
| start_capture | 受支持的配置ID | accepted、task_id；等待真实首帧后完成 |
| stop_capture | task_id或session | 清理结果；在途任务按停止语义处理 |
| capture_snapshot | freshness_ms | frame_id、generation、时间戳、保存路径 |
| detect_snapshot | frame_id、model_id | 实际检测框、类别、分数、模型版本 |
| describe_snapshot | frame_id、用户问题 | 真实VLM输出与推理耗时 |
| get_recent_metrics | 限定窗口与指标名 | 采集、队列、推理与系统统计 |
| create_report | 已完成task_id | 汇总真实结果、图像与限制 |

工具只接受schema中定义的参数，不能把自然语言拼进shell。所有控制工具经ControlRouter进入同一状态机；Agent不单独打开摄像头，不直接操纵ROS或采集模块内部变量。快照文件按任务目录生成，限制数量与总空间。

Agent工具可直接使用本地控制接口，ROS2 Action作为另一任务入口，两者共享TaskManager和任务ID。这样同一任务从CLI、ROS2或Agent发起时，都能查到一致状态。

### 11.3 计划与执行记录

规划输出包括schema_version、request_id、steps和每一步的tool、args、timeout_ms。验证工具名称、参数范围、步骤上限、依赖与资源预算后再执行。初始最大步骤数6，任务队列1，模型生成长度和快照大小有上限，后续根据实测调整。

每一步记录tool_call_id、task_id、frame_id、start、finish、status、error_code和结果摘要。失败、超时或取消均进入终态；有限重试只适用于可重试且幂等的工具。回答引用实际工具返回值，图像描述和检测结果来源分别标记。

例如“看看桌上有什么并保存检测结果”：Agent先查询状态，必要时启动采集，获取新鲜快照，执行固定类别检测，可按请求调用VLM描述，最后保存图像和任务报告。检测模型未覆盖的物体可由VLM描述，但不能把语言模型描述伪装成检测框或置信度。

“为什么最近掉帧”通过get_recent_metrics读取sequence缺口、主动丢弃、队列和资源情况。证据不足时返回需要补采的trace，只有实际工具已采集到的信息进入结论。

### 11.4 8GB设备上的模型调度

先分别测检测模型、Agent模型、ROS图像发布与系统空载的内存，再测目标组合。检测模型常驻，小型语言或视觉模型低频串行运行。共驻不足时采用按需加载、降低图像输入或预算内暂停部分推理；执行策略和对检测的影响需要显示并记录。

采集线程不调用LLM，不等待自然语言生成。GPU任务按采集与持续检测优先的预算调度，但CUDA stream优先级不作为硬实时隔离保证。Agent运行期间测检测P95/P99、采集帧间隔和峰值内存，不能预先承诺“完全没有影响”。

### 11.5 Agent验收

固定至少20条任务集，覆盖查询状态、启动、停止、快照检测、视觉问答、无目标、无设备、模型不可用、非法参数、队列满、超时和取消。模型驱动模式与deterministic开发模式分开统计。

至少保存3条真实端到端轨迹：自然语言拍照检测并生成报告、视觉问答、设备或模型失败后明确返回失败。全部包含真实工具返回、图像ID、模型版本和耗时。记录任务完成率、工具调用正确率、回答依据一致性与错误类型；不要求一次得到固定成功率。

ROS2发起长任务与Agent发起长任务各完成一次演示。两者同时请求时按统一队列返回BUSY或排队状态，避免重复加载模型与重复占用相机。

## 12 日志监控与部署

### 12.1 日志与证据目录

结构化日志使用JSONL，字段包括wall_time、mono_ns、run_id、service、thread、event、request_id、task_id、frame_id、generation、error_code和message。逐帧性能数据单独保存CSV，普通运行日志按事件和采样输出，限制文件大小与保留数量。

每次实验独立目录，保存env.json、config.yaml、model_manifest.json、metrics.csv、events.jsonl和report.md。驱动实验额外保存kernel_commit、config差异、patch与trace；ROS2保存参数和选定topic的bag；Agent保存计划、工具结果和回答。

### 12.2 健康监控与故障处理

| 事件 | 检测方式 | 实际处理 |
|---|---|---|
| 相机无帧 | 最后有效DQBUF时间 | 先告警，再按预算停流重连 |
| 相机断开 | 设备错误与内核记录 | 结束当前generation，释放资源，有限重连 |
| 队列满 | 固定容量与drop计数 | 丢待处理旧帧，不抢占在途slot |
| GPU任务过期 | task deadline与event状态 | 标记结果过期，等待安全回收 |
| CUDA不可恢复错误 | API或异步完成报错 | worker失败，保存现场，按进程边界恢复 |
| Agent模型内存不足 | 加载失败与内存水位 | 拒绝新请求，返回RESOURCE_EXHAUSTED |
| ROS订阅端变慢 | 发布耗时与队列占用 | 限频和有界队列，保持核心可观测 |
| 日志或磁盘到水位 | 磁盘与写入计数 | 停止非必要保存，保留错误与状态 |

初始source软告警可取200ms、硬超时1000ms，具体值依据相机基线校准。推理、模型加载和Agent的预算各自设置，不能共用一个固定超时。重连采用有上限的退避，达到重试预算后保留ERROR并接受明确重试命令。

单进程版本发生进程崩溃时采集也会停止，恢复能力按实测报告。要达到“AI worker崩溃时采集继续”，必须完成独立进程、IPC与异常slot回收后再验收该行为。

### 12.3 部署与展示

原生环境验证通过后，增加systemd unit、配置校验、日志轮转和有界重启。首次部署记录运行用户、工作目录、依赖、video设备权限与model路径。启动自检失败时不得宣告服务ready。

轻量Web展示相机状态、当前图像、检测结果、FPS、P95、主动丢帧、内存温度和Agent任务轨迹。页面同时提供ROS2 topic与服务信息，便于机器人接口演示。Web调用同一ControlRouter，浏览器断开不会触发采集停止。

RTSP作为正式网络预览输出，先限定分辨率与帧率，再评估CPU和尾延迟。若设备确认为Orin Nano，编码方案以其实际硬件能力为准，不能预设NVENC；可先用低帧率预览或软件编码，并测CPU成本。[R13]

## 13 性能指标与实验矩阵

### 13.1 统一测量口径

| 指标 | 定义 | 记录要求 |
|---|---|---|
| 采集FPS | 测量窗口内有效出队帧数除以时长 | 记录实际格式与请求帧率 |
| 完成吞吐 | 真正完成推理的帧数除以时长 | 排除warmup，列出主动丢帧 |
| sequence缺口 | 同一stream内相邻有效序号的跳变 | 与应用主动丢弃分开 |
| 排队耗时 | 任务开始执行减去入队时间 | 单调时钟，同一任务ID |
| GPU阶段耗时 | 对应stream中的event区间 | 标明阶段与包含的操作 |
| 端到端延迟 | 结果发布减有效采集时刻 | 时钟域未确认时改报出队到结果 |
| 结果帧龄 | 当前观察时刻减该帧有效采集时刻 | 保留P50、P95、P99和最大值 |
| 资源 | CPU、RAM、GPU、温度、频率和可读功耗 | 说明采样率、测量范围和缺失项 |
| Agent延迟 | 排队、规划、工具、VLM与总耗时 | 保存tokens、step数与成功状态 |
| 恢复耗时 | 故障被检测到下一次有效服务 | 分别标注注入时刻与检测时刻 |

V4L2 timestamp需要检查flags和来源。不能直接把它与ROS时间、UTC或MCU时间相减。Jetson统一内存下，系统RAM与GPU相关分配可能重叠，内存报告说明统计方法，避免简单相加得到虚假的总占用。

正式性能在固定输入、模型、精度、软件版本、功耗模式、频率策略和热状态下测量。warmup完成后采集至少5分钟，关键配置重复3次；保存各次结果和样本数。P99由足够数量的逐帧样本计算，按时间窗口观察抖动，不能只平均几个分位数。

### 13.2 设计目标

采集以受支持格式下稳定30fps为初始目标，持续检测以20至30fps和P95小于100ms作为待测目标，实际输入shape和模型必须与数字一起报告。目标未达到时先提交瓶颈证据和调整建议，不将设计目标填写为实测性能。

长稳先30分钟，再2小时，成熟版12小时。检查进程存活、有效帧推进、错误计数、RSS与fd趋势、队列上限、过期slot、温度和重连行为。稳定性目标包含持续有效工作，而非仅进程没有退出。

若可读取电源轨，标明测量范围；整机功耗需相应范围的外部或板级测量。每帧能耗由同一测量区间的总能量除以有效完成帧数得到。工具不存在或不支持的指标留空并标注原因，不生成估计实测值。

### 13.3 最少完成的对照

| 实验 | 对照组 | 要回答的问题 |
|---|---|---|
| 采集开销 | 工具基线与C++实现 | 帧行为、CPU和资源管理是否合理 |
| 驱动扩展 | 原版、补丁关闭事件、补丁启用事件 | 信息增量与观测开销分别是多少 |
| 部署正确性 | 参考框架、ONNX、TensorRT FP16 | 精度变化来自哪个环节 |
| 预处理 | CPU与自定义CUDA | 正确性、延迟和CPU成本如何变化 |
| 内存方案 | 至少两种实际可用分配路径 | 复制、同步和缓存的实际代价 |
| 执行并发 | 串行、复用、事件流水线 | 吞吐和尾延迟是否一起改善 |
| ROS2成本 | 关闭、诊断、低频图像、全频图像 | 发布频率与资源成本的关系 |
| Agent干扰 | 检测独立与检测加Agent | 检测尾延迟和内存变化 |

## 14 开发阶段与交付顺序

按完成条件推进。周数为约5至6周的参考排期，依据每天可投入时间调整；不会因为日期到了就自动判定通过。今晚完成M0和M1上游基线首批，后续一次推进一个可验证阶段。

| 阶段 | 参考时间 | 主要交付 | 完成条件 |
|---|---|---|---|
| M0 环境与审计 | 今晚前半小时 | 软件清单、仓库审计、设备能力 | 事实与假设分开，依赖明确 |
| M1 上游基线 | 今晚及第2天 | 固定图、USB输入、原始推理记录 | 未修改上游可复现运行 |
| M2 采集与执行改造 | 第2至3天起 | 输入适配、帧池、TaskManager与指标 | 基于上游增量开发，资源安全 |
| M3 部署基线与驱动准备 | 第1周 | 固定图推理、模块构建、源码调用链 | 正确性初验及原版构建基线 |
| M4 UVC驱动扩展 | 第2周 | tracepoint补丁、解析工具、板端记录 | 新增诊断能力与回退验证 |
| M5 CUDA与异步优化 | 第3周 | kernel、内存对照、stream与event | 数值验证和真实时间线 |
| M6 ROS2集成 | 第4周 | 包、topic、服务、Action、bag | 真实数据与控制闭环 |
| M7 Agent集成 | 第5周 | 模型适配、工具、任务轨迹、报告 | 模型参与的真实工具闭环 |
| M8 系统交付 | 第6周或滚动完成 | Web、systemd、故障报告、12小时长稳 | 可复现启动与完整验收 |

M6和M7属于必须完成的阶段，可在核心接口稳定后提前开展消息与工具schema开发。若时间紧，先完成CLI和低频网络预览，再完成Web与RTSP正式验收，同时保留驱动、GPU、ROS2与Agent的全部验收。

每个阶段交付源码commit、构建命令、实际执行记录、原始数据、结果解释、已知问题及下一步。使用planned、implemented、host_verified、board_verified、blocked五种证据状态；实现与验证按条目分别标记。

## 15 仓库结构与构建约定

下表是projects/edgevision内新增代码的建议职责，不替代上游目录。已有个人仓库时给出映射并复用；能够直接复用的上游模块不重复实现。

| 路径 | 内容 |
|---|---|
| apps | capture_cli、vision_cli、完整服务入口 |
| core | FrameMeta、FrameLease、BoundedQueue、TaskManager |
| capture | V4L2设备封装、协商、采集与恢复 |
| runtime | BufferPool、Executor、CUDA资源封装 |
| backends/tensorrt | engine加载、tensor绑定与结果解析 |
| cuda | 预处理kernel及验证入口 |
| kernel/uvc | 源码版本说明、patch、构建与回退说明 |
| ros2_ws/src | interfaces、ros、bringup三个ROS包 |
| agent | ModelAdapter、Planner、ToolRegistry、Executor |
| configs 与 models | schema、运行参数、manifest；权重单独管理 |
| tools 与 tests | 环境采集、benchmark、trace解析、针对性验证 |
| docs 与 artifacts | 设计和报告；原始实验数据按run_id归档 |

新增系统层采用C++17与CMake，上游保持其原有构建约定，避免无关的全局标准升级。host-only默认不依赖CUDA、TensorRT、ROS2或相机，用于帧池、任务状态和协议验证。Jetson构建显式开启真实后端，ROS2使用colcon；Agent依赖单独管理，避免替换板端CUDA软件栈。

建议提供ENABLE_CUDA、ENABLE_TENSORRT和BUILD_TESTING选项。开启真实后端但依赖缺失时配置失败并给出原因；mock只能显式选择并在日志中显示simulated。README分别列出host、Jetson和ROS2构建入口，首次就能找到最小运行命令。

## 16 测试与故障注入

| 层次 | 必测内容 | 核心断言 |
|---|---|---|
| 纯逻辑 | 队列关闭、slot复用、generation、任务终态 | 不覆盖在途资源，不重复释放 |
| 采集 | 无设备、占用、协商、超时、启停、拔插 | 错误明确，停止可结束，可重新打开 |
| 模型 | 正常图、空结果、错误engine、输入不匹配 | 空检测与失败分开，结果符合参考 |
| CUDA | 边界尺寸、stride、异步依赖、过期任务 | 数值正确，在途buffer不会提前回收 |
| 驱动 | 原版与补丁、事件开关、stop与拔插 | 内核无新增异常，trace字段有效 |
| ROS2 | QoS兼容、慢订阅、服务、Action取消 | 状态真实，有界资源，错误可传播 |
| Agent | 合法工具、非法计划、超时、BUSY、模型失败 | 有限执行，回答基于真实结果 |
| 系统 | 过载、重启、磁盘限制、2小时与12小时 | 持续有效工作，资源趋势可解释 |

先完成与当前阶段对应的必要验证，再向下一阶段推进。host环境能用时以ASan或UBSan检查纯C++资源路径；CUDA与内核使用对应平台实际可用的诊断工具，不将普通host测试当作硬件测试。

每次故障注入记录run_id、注入动作、前置状态、预期行为、实际结果、恢复时间和遗留资源。对GPU hang、OOM等高影响实验优先使用最小复现与受控预算；正常长稳不叠加无关故障，以便解释结果。

## 17 RK3588完整终端与双平台关系

RK3588定位为“多模态边缘AI交互终端”，正式覆盖Boot/U-Boot、kernel/DT/rootfs、全类外设、SH3001/IIO、MAC/PHY与CAN、IMX415加USB双路、RGA/MPP/RTSP、RKNN、RKLLM文本/VLM、ASR/TTS、Qt、ROS2、本地Agent及Linux/RTOS。不能再缩减为IMU加RKNN检测。

| 方向 | Jetson | RK3588 |
|---|---|---|
| 完整系统 | USB视觉、UVC、GPU执行、ROS2、VLM/Agent和网络展示 | Boot到驱动、媒体、网络、语音、HMI及本地AI的终端 |
| 内核与BSP | 匹配L4T构建、UVC扩展、视频buffer生命周期 | 启动适配、全类外设、IIO、media与PHY适配 |
| 计算与媒体 | CUDA/TensorRT、内存与stream/event、软件网络预览 | RKNN/RKLLM、RGA/MPP、双路编码录像与RTSP |
| 交互与机器人 | ROS2视觉任务、本地VLM与Agent工具执行 | Qt/语音、ROS2、本地VLM/Agent与RTOS联动 |
| 深入实验 | GPU流水线、尾延迟、模型与检测资源竞争 | 采样/驱动、启动、网络、媒体与NPU资源竞争 |

两板独立启动、独立部署、独立演示；RK本地模型与Agent不依赖Jetson在线。共用任务schema、错误码、日志与评测语义，各自实现后端；跨板可联合演示，不能替代任一平台本地交付。

不直接复制TensorRT engine、CUDA内存接口或设备树到RK。RK模型图、量化、Runtime/driver与媒体库分别匹配。STM32/FreeRTOS为RK后期正式联动阶段，前面开发不以其先完成为条件。

## 18 最终成果与面试准备

最终应能提供六段短演示：真实USB采集与拔插恢复、UVC驱动新增trace、固定模型端侧预测、CUDA优化前后对照、ROS2真实数据与Action、Agent自然语言调用真实工具。每段对应可定位的源码、配置和原始记录。

面试前逐项回答：一次图像从USB到结果经过哪些buffer；MMAP和DMA-BUF分别解决哪个边界；哪个对象拥有V4L2 fd；GPU没完成时如何防止slot复用；stream与event解决什么依赖；execution context为何需要独立管理；驱动补丁增加了什么实际功能；ROS2时间戳和原始采集时钟如何处理；Agent为何需要任务状态与工具结果校验；哪些性能数据能在目标板复现。

简历描述按实际成果拆成采集驱动、GPU执行、ROS2与Agent三点，数量按版面调整。可以写明“扩展UVC驱动帧完成追踪”“开发C++有界推理执行层”“接入ROS2与视觉任务Agent”，所有优化幅度、稳定时长和成功率以最终报告为依据。

### 18.1 完成后的简历目标

以下按项目完成后的目标写，当前投递按实际状态删改，数值须来自实测：

- 采集与内核：基于jetson-inference二次开发，新增C++ V4L2输入、有界帧池及启停恢复，扩展匹配L4T的UVC帧级追踪，关联内核事件与用户态帧记录分析丢帧和交付延迟。
- GPU执行：完成检测模型TensorRT部署与CUDA预处理改造，利用buffer复用、stream/event构建异步流水线，通过数值回归和Nsight时间线分析内存、吞吐与尾延迟。
- 机器人与多模态系统：集成ROS2图像/检测/诊断、服务与Action，以及本地VLM和Agent真实工具执行，完成网络预览、结构化日志、部署恢复与长期运行验证。

## 19 参考资料与适用版本

技术资料核对日期为2026年11月15日。以下链接用于理解接口与平台约束；具体操作以板端版本、相应release源码和实际头文件为准。不能根据最新文档中的新API直接升级当前设备依赖。

- [R1] NVIDIA Jetson Linux 36.4.3 Release Notes。确认对应平台、内核与系统基线。[官方文档](https://docs.nvidia.com/jetson/archives/r36.4.3/ReleaseNotes/Jetson_Linux_Release_Notes_r36.4.3.pdf)
- [R2] NVIDIA Jetson Linux 36.4.3 Kernel Customization。内核、模块及设备树构建流程。[官方文档](https://docs.nvidia.com/jetson/archives/r36.4.3/DeveloperGuide/SD/Kernel/KernelCustomization.html)
- [R3] Linux V4L2 Streaming I/O Memory Mapping。MMAP及buffer队列接口。[内核文档](https://docs.kernel.org/userspace-api/media/v4l/mmap.html)
- [R4] Linux 5.15 videobuf2 API。缓冲区与allocator生命周期。[内核文档](https://docs.kernel.org/5.15/driver-api/media/v4l2-videobuf2.html)
- [R5] CUDA for Tegra 12.6.3。共享DRAM、内存类型、缓存与平台功能限制。[官方文档](https://docs.nvidia.com/cuda/archive/12.6.3/cuda-for-tegra-appnote/index.html)
- [R6] NVIDIA TensorRT 10系列能力与性能优化。context、enqueueV3与多stream；代码与参数以本机版本为准。[执行接口](https://docs.nvidia.com/deeplearning/tensorrt/10.x.x/architecture/capabilities.html) [性能优化](https://docs.nvidia.com/deeplearning/tensorrt/10.x.x/performance/optimization.html)
- [R7] Ultralytics YOLOv8。作为可冻结版本的小模型候选。[官方文档](https://docs.ultralytics.com/models/yolov8/)
- [R8] Linux V4L2 DMA buffer importing。驱动支持检查和设备间共享。[内核文档](https://docs.kernel.org/userspace-api/media/v4l/dmabuf.html)
- [R9] Linux Dynamic DMA mapping Guide。CPU虚拟地址、物理地址与DMA地址。[内核文档](https://docs.kernel.org/core-api/dma-api-howto.html)
- [R10] ROS2 Humble官方发布说明。Ubuntu 22.04的arm64与amd64为Tier 1目标平台。[官方发布说明](https://discourse.ros.org/t/ros-2-humble-hawksbill-released/25729)
- [R11] ROS2 Humble QoS说明。Sensor Data profile、可靠性与兼容性。[官方文档源文件](https://raw.githubusercontent.com/ros2/ros2_documentation/humble/source/Concepts/Intermediate/About-Quality-of-Service-Settings.rst)
- [R12] CUDA C++ Best Practices 12.6.3。异步传输、stream与计算重叠。[官方文档](https://docs.nvidia.com/cuda/archive/12.6.3/cuda-c-best-practices-guide/index.html)
- [R13] NVIDIA Orin Nano Software Encode。预览与编码方案的设备边界。[官方文档](https://docs.nvidia.com/jetson/archives/r36.4.3/DeveloperGuide/SD/Multimedia/SoftwareEncodeInOrinNano.html)

项目内还需记录当前个人仓库审计结果、相机描述符与能力清单、匹配内核源码版本、模型manifest和EdgeQwen实际可运行接口。现有实习采集系统材料用于理解模块边界、状态回流和故障诊断，个人项目的已实现结论由自己的代码与板端记录支撑。

- [R14] jetson-inference主项目与配套模块。[主仓库](https://github.com/dusty-nv/jetson-inference) [本次核查提交](https://github.com/dusty-nv/jetson-inference/tree/45da40a8f3c180191b269f57f736caaa025b8a69)
- [R15] jetson-utils相机与CUDA工具。[配套源码](https://github.com/dusty-nv/jetson-utils/tree/6d5471c97dad443776d2339569a4d1145f034a9e)
- [R16] ros_deep_learning节点与Humble相关说明。[仓库](https://github.com/dusty-nv/ros_deep_learning)
- [R17] tensorNet.cpp中的版本分支与执行路径。[源码](https://github.com/dusty-nv/jetson-inference/blob/45da40a8f3c180191b269f57f736caaa025b8a69/c/tensorNet.cpp)
- [R18] L4T 36.4.3镜像缺失的历史issue；仅作为适配检查项。[Issue 1927](https://github.com/dusty-nv/jetson-inference/issues/1927)
- [R19] NanoLLM多模态Agent结构参考。[仓库](https://github.com/dusty-nv/NanoLLM)
- [R20] ROS2视觉语言模型节点参考。[仓库](https://github.com/NVIDIA-AI-IOT/ros2_nanollm)

- [R21] 本地视觉语言模型候选。[Qwen3-VL-2B-Instruct](https://huggingface.co/Qwen/Qwen3-VL-2B-Instruct)
