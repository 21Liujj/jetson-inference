# Checkpoint · 2026-11-16 · M2 基础验收完成，继续 M3

用户明确要求持续施工，不在状态答复或M2验收后主动收工。先读AGENTS及相关领域SKILL；
问题唯一正文debugging_journal.md。progress.md为验收真相源。

## 已有证据

- 主线指定jetson-inference个人fork，edgevision/v3.1；静态工具式网页走gh-pages。
- M1原版TRT10崩溃保留，afee249修复固定SSD路径；30min USB 53,898次推理。
- M2 R3：100次启停（3次受限启动重试）、30min纯采集53,999帧，原失败保留。
- R8：新V4L2输入接上游DetectNet，复用M1 engine；60s完成1,802次，非精度/性能结论。
- R10：统一initialize/submit/query/collect/cancel_pending/shutdown，检测与启停共用
  一个TaskManager。Windows9/9、Jetson ASan/UBSan/LSan11/11；真实GPU7项生命周期
  与模型失败负例通过。逻辑终态仍保留物理执行槽，真实event完成后才归还lease。
- 用户配合联合拔插时真实GPU event仍pending；READY队列0、在途pool1。原任务775
  物理完成后FAILED/CAPTURE_NOT_RUNNING且无结果；3次有限重开至gen4，新会话两个
  检测DONE，首帧恢复7.5720s。参数5次/30s，测试历史4096；不是默认3次/15s结论。
  本轮776次GPU提交/完成、1个故障任务无结果，最后IDLE/UNLOADED、pool0/maps0正常退出。
- 显式CUDA测试负载请求200ms，默认构建关闭，不是M5预处理或性能优化。
- 最新证据evidence/2026-11-16-executor/，原始源码/错误/复测与哈希已收本机；
  按实际源码分版本验收，不把R3长采集当R10最终系统长稳。此前证据目录继续保留。

## 现场与构建身份

板端/home/nvidia/jetson-inference-edgevision的Git HEAD仍是上游冻结父提交加SFTP源码，
禁止blanket pull/reset/clean。模型、build-baseline和原两个工作区均保留。
R10源码包SHA256：fc23d0a30fee0c8c329608aa0c9f2f648f9d52a9bf87c167e813d2229077b5c5。
R10主执行probe SHA：321f0d64d51e711d1969638a8bcab57b8de9fb7d24fe9f6be5e15b675cc6cb2b。
上游库仍为build-baseline/aarch64，M1 engine SHA为3cb11235…ab2fc4。
模型manifest已纠正workspace：日志请求512MiB，TRT10上游未显式应用cap，见EV-GPU-002。

native为build/inference-native，采集原生为build/capture-native，目标sanitizer为
build/capture-asan。nvcc不在非登录PATH，显式CMAKE_CUDA_COMPILER=/usr/local/cuda/bin/nvcc。
验收CUDA target使用Shared runtime，不能带入R9混合cudart设置。Capture/GPU测试已退出、
视频无owner；SSH工具会话仍在。未切换或安装内核模块、升级JetPack、USB reset或刷固件。
下一次开设备前重新核对稳定设备与owner。

## 已开始的 M3 准备

- 已只读核对目标5.15.148-tegra / L4T36.4.3、运行config、modinfo、headers与
  Module.symvers。headers没有UVC .c。内核编译器Bootlin11.3，原生gcc11.4，需记录差异。
- 官方R36.4.3 public_sources.tbz2已下载到本机artifacts/m3-kernel-r3643，
  SHA256 2c177804679e3ed650dabec6fa958388579896f170570c6171a1b6c386669216；
  嵌套kernel_src SHA256 0c314c5174d07149194e0a5a50d0f14a4ee5a4fe2cd1f382b471624cc1aa5184。
  完整UVC源码13文件已提取，原Makefile版本5.15.148；本机计算哈希不是厂商发布哈希。
- board inventory在artifacts/2026-11-16/m3-kernel-inventory；下载工具fetch_kernel_sources.py。
- 旧board /home/nvidia/edgevisionrt/models 有yolov8n.pt、onnx与fp16 engine，已只读哈希；
  后续先审计导出/预处理/I/O，再决定复用，不继承旧性能或mAP。PC有numpy/Pillow，
  还没有onnx/onnxruntime/torch/cv2；新依赖放隔离目录，别升级板端CUDA软件栈。

## 当前继续

1. 把匹配的原UVC源码与manifest传板端，使用现有headers/Module.symvers构建未修改模块；
   核对vermagic、srcversion、导入符号CRC，准备原模块备份与回退，之后才做实际加载验证。
2. 审计既有YOLO模型、固定20图与参考输出；补最终模型manifest和数值/准确性基线。
3. M3未完成。之后M4真实UVC trace补丁/安装回退、M5数值与Nsight、M6 ROS2、M7真实
   本地VLM/Agent、M8实时Web/RTSP/部署及2h/12h都完整保留。vision_msgs仍缺。
