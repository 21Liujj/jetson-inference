---
name: edgevision-ros2
description: 为 EdgeVision 接入或排查 ROS2 图像、检测、诊断、服务、Action、QoS 和 rosbag 时使用。
---

# ROS2 接口

先读 progress 的 M6 状态与施工方案第 10 节；现有 `ros/` 固定为上游子模块，
其存在不等于新主线已经完成机器人接口。只读本次相关节点或接口文件。

- 目标环境是 Humble；重新检查发行版与 vision_msgs，安装 ROS 和项目集成分开验收。
- 图像、检测和诊断关联内部 frame_id、capture generation 与时间来源；
  ROS header.frame_id 表示坐标系，不能拿内部数字帧号替代。ROS 时间与
  CLOCK_MONOTONIC 不直接相减；转换或对齐方法必须可解释。
- service/Action/CLI/Web/Agent 均走 ControlRouter，不另开相机。
  服务受理不是任务成功；Action 要观察成功、失败、超时、取消及物理资源回收。
- 实测传感器 QoS、慢订阅与有界队列，不能让 ROS 发布阻塞采集热路径。
  模型失败和相机失败分别诊断，空检测不能被当作采集错误。
- 保存真实 topic、服务、Action 和小规模 bag 的验证记录。完整 bag 与私有图像
  不入 Git；schema 或模拟消息不能代表真机联调通过。

缺少实现时按文档继续开发并保持 planned/partial，避免建立空节点后直接改成完成。
接口影响控制状态时同时选读 core-control skill。
