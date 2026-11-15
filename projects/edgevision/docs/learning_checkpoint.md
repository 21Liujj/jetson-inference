# Checkpoint · 2026-11-15 晚间施工，11-16 凌晨收尾

用户明确要求：同步当前内容到 GitHub 后停止，明天再继续。今晚不启动新开发或长测。
先读根 AGENTS.md，再按领域加载 .agents/skills 中一个 SKILL；问题统一在 debugging_journal.md。

## 已有证据

- 主仓库为指定 jetson-inference 的个人 fork，默认 edgevision/v3.1，gh-pages 展示。
- M0 已审计；M1 原版 TRT10 SIGSEGV 保留，afee249 修复指定模型路径。
  修复后 30min 真实相机推理 53,898 次，正常退出；不是正式性能 benchmark。
- M2 当前源码输入为 m2-capture-r3.zip，SHA256
  2e3fc65c63225cba073f822b0046e3f5f13aab4d860415cfa9b191e28d4a9684。
  Windows core 5/5，Jetson ASan/UBSan/LSan 7/7，真实短采集/慢消费者/持有帧/
  无设备/占用测试通过。100 次真实启停、300 帧，共 3 次受限启动重试。
- R2 原始 37 轮后 S_FMT EIO 保留；UVC probe -75 根因未确认。
  R3 是有限恢复与复测，不是内核缺陷已修复。详见 EV-CAP-002。
- 新后端证据在 evidence/2026-11-15-m2-capture/，原始 81 文件 ZIP、源码、日志、
  metadata 与哈希已回收到本机；不含私有相机图片。两旧仓库未修改。

## 板端与代码现场

板端 /home/nvidia/jetson-inference-edgevision 的 Git HEAD 仍是上游固定父提交，
其上有本轮 c/tensorNet.cpp 与 SFTP 传入项目源码；执行版本以实际输入 ZIP/哈希为准。
禁止在其证据、模型与 build-baseline 上 blanket pull/reset/clean。
原 /home/nvidia/jetson-inference 和 /home/nvidia/edgevisionrt 保留。

收尾检查：任务自有采集/验收进程结束，视频节点无占用，旧 edgevision.service 为 IDLE。
没有替换内核或模块、升级 JetPack/CUDA、刷固件、重置 USB，也没有代用户关机。
SSH 已关闭。新代码在本机/公开 fork；下次开工重新确认 SSH、L4T、稳定相机标识和 owner 后再开设备。

## 明天从哪里继续

1. 阅读 m2-capture.md、progress 的 M2 行和 journal 的 EV-CAP-002；运行中断连恢复
   不能直接复用“启动重试已验”的结论。保留原失败及所有重试计数。
2. 完成新后端 30min 采集、运行中拔插/有限重连、HealthMonitor。
3. 接入上游 DetectNet 个人输入适配，保留 GStreamer 对照；GPU 生命周期按核心契约。
4. 关闭剩余 M2 门槛后进入 M3 冻结回归集与匹配内核源码准备。

M3–M8 仍完整保留：真实 UVC 源码扩展/安装回退、CUDA 数值与 Nsight、ROS2、
本地 VLM/模型驱动 Agent、实时 Web/RTSP、部署和 2h/12h 等验收尚未完成。
vision_msgs 仍缺；静态展示不代表实时控制台；简历只采用已验证事实。
