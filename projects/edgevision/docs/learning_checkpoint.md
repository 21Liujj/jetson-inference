# Checkpoint · 2026-11-16 · M3/M4验收完成，持续M5→M6→M7

用户要求持续实际施工：16:30–19:30健身期间继续，19:30回来检查。
不要在状态答复、里程碑或证据归档后主动收工；不创建任务监视automation。
当前方案M5/M6/M7无需人工，只有硬件掉线等才联系。先读AGENTS及相关领域skill，
问题唯一正文debugging_journal.md，验收真相progress.md；M0–M8范围保留。

## Git与现场

- fork jetson-inference，edgevision/v3.1。M2源码860c6d3已推；M4代码/原始证据1229a84
  在16:27已推GitHub。M3模型源码/证据本机齐备，准备提交。
- gh-pages上次692df71仍展示M2完成/M3准备；后续只更新工具式页面事实状态。
- 板端/home/nvidia/jetson-inference-edgevision为冻结上游父HEAD加SFTP源码，禁止pull/reset/clean。
  原两个仓库及未提交工作保留。Windows上游大小写噪声不暂存。
- 已实际进行原UVC→自编原版→原UVC、trace模块矩阵/真实拔插→原UVC。
  目前系统UVC sysfs Build ID 4f3253a8a815a048b37be4908a51db4ef5b66053。
  taint4096→12288，回退不清除；sig_enforce=N原策略未改，未重启/刷固件/改JetPack。
- SSH私有helper会话82434、26078，凭据只在内存。稳定camera by-id见run.json。
  原edgevisionrt服务保留，主线一个相机owner；板端无rg，用grep等回退。

## 已验事实

- M1原版TRT10崩溃保留、afee249修复；30min真实推理53898次。
- M2 R3：100启停、30min采集53999帧；R8直接输入60s1802次；R10统一任务、
  真实GPU逻辑超时/取消/stop/shutdown及联合USB恢复见相应证据目录。
- M3原UVC 155导入CRC/vermagic/srcversion匹配，三段5s151/150/151帧、真实回退。
- M3 YOLO ONNX e4b651…dea7940，metadata8.3.159/torch2.5/opset12。
  新engine2925e839f38ff81f186db6284e6e369c23399fdd50914b10526fd8a8c0474519。
  200张子集ORT AP.394822295→TRT.394353343，下降.046895个百分点；框199/200。
  图470924分数.252635→.249390跨.25阈值，原输出保留，未调整门槛。
- CPU letterbox24例含RGB行padding/奇数尺寸/截断，与PC逐字节相同；host10/10，
  target ASan/UBSan/LSan12/12（72.24s）。60s视频1803次全有效空，非相机精度证明；
  发布1802，停止时1结果失效，pool0。R5 YOLO/SSD任务生命周期各7项通过。
- R4测试错误要求restart只增1代，原失败保留；R5核对1+start_retries，生产策略未改。
  最新完整源码R5 ZIP76文件SHA4bc711dcdbd5a7025b0c3ad377872c5b3dabe39fabe8ae0ac747ac8e45e8e062，
  全部与板端实际文件核对。模型证据evidence/2026-11-16-yolo已收齐。
- M4补丁4文件、172导入CRC匹配。原版/关闭/开启各正常、慢消费、启停，两轮矩阵通过。
  R1并行engine构建；R2仅观察开销，不宣称零开销。慢消费241=80消费+159替换+2停止；
  另有668字节error1内部回收无DQBUF。实际拔插7.3799s、1803消费全关联，
  用户gen1→4映射kernel stream1→2。原始trace/回退在evidence/2026-11-16-uvc-trace。

## 当前M5与后续

- 主线仍为修复tensorNet/DetectNet。YoloV8Coco显式选择，SSD默认保留。
  M3 YOLO路径是GPU转RGB→下载CPU→OpenCV letterbox/CHW→上传GPU，串行屏障。
- Nsight2024.5.4、CUDA12.6、OpenCV4.10已核对，nvpmodel MAXN_SUPER(mode2)。
  P0初步profile在板端artifacts/2026-11-16/m5-p0-diagnostic-r1/p0.nsys-rep（2.3MB），
  监督exit0、stats.csv已生成；无CPU采样、自定义NVTX阶段或正式锁频比较。
- 下一步读P0统计，冻结融合RGB letterbox/CHW/normalize数值门槛，加NVTX、固定池、
  pinned→device和mapped两路径，P0/P1/P2事件依赖及Nsight/吞吐/尾延迟比较。
  一个推理context避免并发复用，逻辑取消不提前释放GPU仍在访问的内存。
- M6 ROS2 Humble同进程封装统一核心，限频图像分支，一个相机owner；vision_msgs缺失。
- M7尚未验收；HF cache有models--Qwen--Qwen3-VL-2B-Instruct，需核对权重完整性、
  环境与真实推理，不把OWLViT检测当VLM问答。
- M8实时Web/RTSP、部署恢复、30min/2h/12h完整保留；静态页不是实时控制台。
