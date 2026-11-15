---
name: edgevision-evidence
description: 记录工程故障、核验原始证据、整理验收进度、维护贡献边界或准备面试讲解时使用。
---

# 工程证据与经验

问题正文唯一位置是
[debugging_journal.md](../../../projects/edgevision/docs/debugging_journal.md)。
先搜索关键词或事件 ID，只读相关条目；报告、skill 和进度表链接该条，不另建一套故障正文。

每个问题使用稳定 ID，记录环境/源码版本、触发动作、预期与实际、最小复现、原始
日志/命令/哈希、已证实原因、假设、处理、复测、未解决项与面试追问。
“已证实”“参考源码推断”“尚未确认”分开。不能把重试后成功写成底层缺陷已修复。

- 原始失败不覆盖，修复/重试另建 run_id。保存真实退出码、stderr、参数和实际
  执行源码；board 工作区 Git HEAD 不一定反映 SFTP 传入的代码。
- curated evidence 在 `projects/edgevision/evidence/`；较大的原始文本用 ZIP。
  不含密码、token、权重、engine、私有照片或完整 bag。
- 哈希验证覆盖本地、远端和实际提交的 Git blob。evidence 的 -text 属性用于保留
  原字节；别只核对工作区文件却让 index 的换行转换破坏 manifest。
- `docs/progress.md` 是验收真相源，`docs/learning_checkpoint.md` 是续施工入口，
  `UPSTREAM.md` 是贡献边界。三者引用 journal 和 evidence，不复制完整故障历史。
- host、target fixture、真实相机、真实 GPU、性能、长稳分开记账。
  旧仓库里程碑与新主线定义不同，不自动继承结论；简历仅采用可定位的事实。

新增技能只保留会改变下一位 Agent 决策的仓库知识；先更新相关领域 SKILL，
根 AGENTS 只保留路由与跨领域约束，不堆叠过程日志。
