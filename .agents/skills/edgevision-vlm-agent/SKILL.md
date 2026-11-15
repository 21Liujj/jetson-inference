---
name: edgevision-vlm-agent
description: 开发 EdgeVision 本地 VLM、模型驱动 Agent、工具 schema、任务计划和报告依据时使用。
---

# 本地视觉任务 Agent

这是开发技能，不是产品运行时工具的实现。先读 progress 的 M7 状态和施工方案
第 11 节，确定真实模型、执行工具与未完成项，再加载相关源码。

- 本地 VLM 独立验证后再接 Agent；固定模型版本、输入处理、内存预算和加载日志。
  规则匹配、固定回答或工具假结果不能充当模型驱动 Agent 验收。
- 运行时工具参数走明确 schema，所有控制走同一个 ControlRouter/TaskManager。
  不把自然语言拼入 shell，不绕过队列另开相机或直接改内部状态。
- 计划最多 6 步、任务排队初值 1，步骤和整体均有 deadline。只对可重试且幂等的
  工具有限重试；取消和过期不得发布迟到结果或释放未完成的 GPU 内存。
- 每步保留 tool_call_id、task_id、generation、frame_id、时间、终态、错误和摘要。
  回答引用实际工具数据，检测与 VLM 描述来源分别标记。
- 快照/报告有目录、数量、空间与保留预算，模型及图像不入 Git。
  用文档要求的真实任务集和调用轨迹验收，不能只验证 schema 格式。

开发 Agent 所用的仓库 SKILL 与产品 Agent 所调用的运行时工具分开维护；
新增工具的真实入口形成后再补该入口的调用说明，禁止用说明文件冒充已实现功能。
