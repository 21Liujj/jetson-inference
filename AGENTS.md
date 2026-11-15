# EdgeVision · Agent 开发入口

本仓库是 `dusty-nv/jetson-inference` 的个人 fork，按 V3.1 施工。先读本文件，
再按任务加载下表的一个领域技能和必要源码；不要为普通修改全量扫描仓库或读取所有技能。
这里的 SKILL 服务于开发 Agent，不代表产品运行时 VLM/工具已经实现。

## 真相与续施工

- 验收状态：[progress](projects/edgevision/docs/progress.md)，只读相关阶段。
- 当前现场与下一步：[learning_checkpoint](projects/edgevision/docs/learning_checkpoint.md)。
- 工程问题唯一正文：[debugging_journal](projects/edgevision/docs/debugging_journal.md)，按 ID/关键词查阅。
- 复用、修改、新增：[UPSTREAM.md](UPSTREAM.md)；用户规范在 `projects/edgevision/docs/spec/`，按领域选读。

## 按任务选技能

| 领域 | 只在相关任务时读取 |
|---|---|
| 帧池、队列、任务与状态机 | [core-control](.agents/skills/edgevision-core-control/SKILL.md) |
| USB / V4L2 / MMAP / 启停恢复 | [capture](.agents/skills/edgevision-capture/SKILL.md) |
| L4T / UVC / 内核追踪与回退 | [kernel-trace](.agents/skills/edgevision-kernel-trace/SKILL.md) |
| TensorRT / CUDA / 推理适配与优化 | [gpu-inference](.agents/skills/edgevision-gpu-inference/SKILL.md) |
| ROS2 / 服务 / Action / QoS | [ros2](.agents/skills/edgevision-ros2/SKILL.md) |
| 本地 VLM / Agent / 真实工具 | [vlm-agent](.agents/skills/edgevision-vlm-agent/SKILL.md) |
| Web / RTSP / 部署 / 长稳 | [delivery](.agents/skills/edgevision-delivery/SKILL.md) |
| 故障沉淀 / 证据 / 贡献与简历 | [evidence](.agents/skills/edgevision-evidence/SKILL.md) |

## 跨领域约束

- 保留上游布局、许可证和 `README.upstream.md`。冻结上游 `45da40a8` 及 gitlinks；
  主线个人代码在 `projects/edgevision`，C++17 按 target 启用，不改上游全局标准。
  修改子模块须先提交个人 fork，再更新 gitlink。
- 一个相机 owner；控制入口统一。GPU 物理完成前保留在途内存；逻辑超时不归还 slot。
  host fixture、真实硬件、性能和长稳分别验收。M0–M8 范围不因投递时间缩减。
- 原来的 EdgeVisionRT、jetson-stm32F407 及未提交成果保留；迁移记录精确 donor hash。
  板端工作区可能是固定 HEAD 加 SFTP 源码，以实际构建输入和哈希为准，禁止 blanket clean/reset。
- 不提交凭据、权重、engine、私有照片或完整 bag。原始失败不覆盖；问题正文只写固定 journal。
  改动完成后按实际结果更新 progress、checkpoint、贡献记录和相关技能，不在本文件堆历史日志。
- 当前 Windows checkout 有上游大小写碰撞噪声（`docs/html/XML_8h*`、Python Jetson 路径、utils）。
  不暂存、不反转这些噪声；精确暂存个人改动。真实问题和未完成项必须如实记录。

用户当前指令与已有授权优先；领域技能不扩大授权、不凭空增加确认流程。
