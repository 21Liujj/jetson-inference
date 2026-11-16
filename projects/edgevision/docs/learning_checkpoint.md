# Checkpoint · 2026-11-16 · 已按用户要求暂停

## 用户最新安排

今晚不再连接Jetson，明天外出，周四面试；可能周四以后继续Jetson，并从周四开始RK3588。
收尾同步后停止开发，等用户明确续做。不要自动启动测试、创建automation、新仓库或开始RK3588。
已解释：视觉采集/驱动/推理/ROS2核心主体比较完整；完整Agent与M8交付尚未完成。

## 现场已收尾

- 自有板端测试均退出，最后仅原有edgevisiond服务；原服务未动。22:29已通知可正常关机后断电。
- M5/M7日志已下载核验；SSH helper 26078/82434均退出，凭据未入库。本机localhost8769预览也已停止。
- Windows XML_8h、Python Jetson大小写路径和utils是既有噪声，不暂存、不反转。
- 板端/home/nvidia/jetson-inference-edgevision为固定父HEAD+SFTP源码，禁止pull/reset/clean；以source ZIP/hash为准。

## 代码与证据

- edgevision/v3.1已包含ef4f2f2正式M5对照、de91f03网页事实和03622eb M7部分工具链。
- 最后补交interview-checkpoint.md、这个暂停检查点及实际运行过的二元完成判定prompt实验。
- gh-pages 53402d9已推，在线HTTP200且新进度/数字匹配。HTML/资源/JS检查通过；CUA因kernel assets路径失败，未做新视觉复核，已completion_summary结束。
- progress.md为验收表；问题唯一正文debugging_journal.md，重点EV-EVIDENCE-003、EV-VLM-001/002/003。
- M5 evidence/2026-11-16-gpu-formal：五配置各3×300秒，646,305次完成，资源利用率/RSS/功耗对照待补。
- M7 evidence/2026-11-16-agent-checkpoint：121文件raw ZIP，SHA40d0e34e91237ef6eed9eafa55fdd368406c4ddee9b04e70f0b5b1722e900195。
- M7 R2源119文件SHA2b85768ef6acf9d2427e03a240d062c114d935bc837cbd16729a4d3a758050f7，精确Git blob比对后提交。

## M7真实状态

- R1 Windows CTest13/13；R2 target原生CTest15/15（非sanitizer）及Humble三包通过；R1旧检测真实GPU生命周期7项通过。
- build/m7-native-r1及build/install ros2-m7-r1目录复用构建R2，目录名不是源码身份。
- 实际node路径install/ros2-m7-r1/edgevision_ros/lib/edgevision_ros/edgevision_node。
- R4 snapshot→同帧detect→真实model plan→同帧VQA→resume均返回，context释放2次，最后exit0/物理shutdown确认。
- 总体Agent仍FAILED：中文短提示选了不必要的start_capture，被STATE_CONFLICT拒绝。先前英文重复get_status达到步数上限。
- R4系统MemAvailable最低158,412KiB，资源余量仍小，不是正式预算或干扰通过。
- 固定tests/agent_tasks_v1.json共22任务未正式整批跑，3完整轨迹、模型取消/超时/竞争、正式资源干扰待验。
- registered-tool-id-v2由真实模型选编号，host按schema造参数，zh-next-operation-v3为当前提示。不能用规则/预置计划冒充模型闭环。
- tools/vlm_planner_probe.py完成判定实验仅4/6且有过早完成，未接产品，不能当修复或正式任务成绩。

## 续做注意

- AgentTools/SnapshotStore/VlmProcess和Python Agent共享同一TaskManager/相机owner；最多6步，一活跃一待排、64保留记录；快照16张/32MiB raw+PNG预算。
- donor /home/nvidia/edgeqwen只读；owned bridge build/vlm-native-r1/edgevision_vlm_bridge，SHA45c3f30351029b877339172ed19792fa6a9351219d8d402d28d37683969943e9。
- assets-r3保持original profile、排除不用的可选action；KV/input缩减被固定profile拒绝，不得跳过检查。
- Qwen3-VL-2B架构，cached engine完整导出谱系未重建；base权重/HF cache匹配不能证明engine来源，不继承旧性能。
- context释放仅限唯一CUDA owner销毁全部对象、前序物理完成后；绝不reset在途工作。
- 私有原始结果在父目录.tools及artifacts/2026-11-16，读保存JSON，别手抄PTY换行hash。

恢复时先读interview-checkpoint.md、m7-agent.md，确认用户续Jetson还是开RK3588。
Jetson优先修规划语义/资源拒绝边界，再跑22任务/三轨迹/取消竞争。另补M5资源、M6正式性能，
最后M8实时Web/RTSP、部署恢复及30min/2h/12h。静态网页不是实时控制台，组件长稳不是最终全栈长稳。
