# Checkpoint · 2026-11-16 · M2 恢复与采集验收

用户已重新上电并明确继续施工，前次暂停解除。先读根 AGENTS，再按领域读一个 SKILL；
问题唯一正文在 debugging_journal.md。当前阶段和所有待验项以 progress.md 为准。

## 已验证事实

- 指定 jetson-inference 个人 fork，主线 edgevision/v3.1，gh-pages 为静态工具式展示。
- M1：原版 TRT10 崩溃保留；afee249 修复固定 SSD 路径，30min USB 53,898 次推理。
- M2 R3：真实 100 次启停（含 3 次启动重试），30min 纯采集 53,999 帧、无序号缺口或
  应用丢弃。60s 后该窗口 RSS 4,272 KiB、fd7；停止 fd6 / 视频映射0。
- R4：Windows 7/7、目标 ASan/UBSan/LSan 9/9；运行中有限恢复、HealthMonitor。
  真实拔插 4 次重开到 gen5，检测故障到有效首帧 12.3811s；参数5次/30s。
- R5 只改恢复 probe：操作窗口600s，故障后 stop，实际节点返回后进程继续观察5s，
  保持 IDLE、gen1、无活动 worker/设备资源，正常退出。
- R8 新输入接入上游 DetectNet，复用 M1 engine，60s完成1,802次；模型缺失负例为
  capture=RUNNING/model=ERROR。Windows8/8、目标sanitizer10/10。R6/R7不同engine
  记录保留；R8来源sha256见下方。当前没有精度/异步优化/正式性能结论。
- R2 的 S_FMT EIO / UVC probe -75、R4 无效180s人工窗口均保留。有限恢复不代表
  底层驱动缺陷已修复。固定 journal：EV-CAP-002/003、EV-TEST-001、EV-METRIC-001。
- 新证据：evidence/2026-11-16-capture-30min 与 2026-11-16-runtime-recovery。
  原始源码与日志逐文件哈希验证；不含私有图像、权重、engine 或凭据。

## 现场与执行身份

板端 /home/nvidia/jetson-inference-edgevision 的 Git HEAD 是冻结的上游父提交，
其上为 SFTP 传入源码。不能用 HEAD 冒充实际执行版本，不能 blanket pull/reset/clean。
R3 源码 ZIP sha256：2e3fc65c63225cba073f822b0046e3f5f13aab4d860415cfa9b191e28d4a9684。
R4：197cf2bf9b4a3d6fcafb1438c3109eac48169fa87db36bff9e215e8e51debb90。
R5：282c9e7bf75c412d882f0ea506b079afb2c39ac31d98742123b9f91a13b8d028。
R8：183a9b41fada3c457f894f06e1bf831c6a5ebab02e62f353481fe09aa4b3ee7b。
R3 30min 不能作为 R4–R8 长稳。原生构建目录 build/capture-native，sanitizer 为
build/capture-asan；不要猜成 jetson-native。

相机按稳定 by-id 打开；旧 edgevision.service active、IDLE。R5 退出后视频无 owner，
R8停止后模型UNLOADED、视频无owner，本轮 SSH 会话仍在，后续开发继续。原两个项目及其未提交工作保留。无内核替换、JetPack
升级、USB reset 或刷固件。下一次开设备前重新核对 owner。

## 当前下一步

1. 串行输入已验，见 m2-inference.md 与 evidence/2026-11-16-inference-adapter；
   build/inference-native 链接固定 build-baseline/aarch64 库，保留 GStreamer 对照。
2. 接入完整执行层 initialize/submit/query/collect/cancel_pending/shutdown，复用
   公共任务边界；GPU在途超时、取消、联合拔插与停止仍需独立实测。
3. 关闭剩余 M2 门槛，再进入 M3 固定20图回归、带标注集合与匹配内核源码准备。

M3–M8 完整保留；UVC 补丁安装回退、数值与 Nsight、ROS2、真实本地 VLM/Agent、
实时 Web/RTSP、部署及2h/12h长稳都还未通过。vision_msgs 仍缺。静态页不是设备控制台。
