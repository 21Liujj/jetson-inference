# Checkpoint · 2026-11-16 22:31 · M7底层已验，完整Agent未通过，板端已收尾

用户要求持续施工，不能状态回复后停工。最新问题“M7怎么样、Agent用途”已在commentary说明：
低层真实工具链已贯通，完整Agent尚未验收；产品Agent是板上自然语言视觉任务助手。继续工作。
用户约22:30回宿舍，之后可重新供电联网；板端自有测试已全部退出，121文件日志包已下载核验；22:29已通知可正常关机后断电。运输期间只在PC开发。
无主动子Agent授权，不spawn；不要创建监控automation。M0–M8全范围、正式性能和最终30min/2h/12h保留。

## 权威入口与现场

- 仓库jetson-inference，分支edgevision/v3.1。最新已推ef4f2f2（正式M5时长/重复对照）；此前f69e74a M6、9584f78 M5、1229a84 M4、73cf63a M3。
- progress.md为验收表；问题唯一正文debugging_journal.md，最近EV-EVIDENCE-003、EV-VLM-001/002/003。
- 板端/home/nvidia/jetson-inference-edgevision是固定父HEAD+SFTP源码，禁止pull/reset/clean。按实际source ZIP/hash核对。
- SSH helper PTY：26078与82434均闲置；用户正在准备关机运输，暂不启动板端工作。私有脚本在父目录.tools，密码只在内存。
- 板原UVC模块已恢复，Build ID 4f3253a8a815a048b37be4908a51db4ef5b66053，taint12288；未改SDK/网络/启动项。
- 保留旧edgevisiond pid946等原服务。只停止自己启动的测试进程。Windows XML/Jetson大小写/utils噪声不暂存或反转。
- Python主运行时位于WindowsApps OpenAI.CodexPrimaryRuntime.v26-1112-611-0.../dependencies/python/python.exe；g++在D:/msys64/ucrt64/bin。

## M5正式批次已核验并公开

- 20:01:19–21:17:07，五配置各3×300秒，排除每轮60次warmup，646,305次完成。逐CSV行、计数、nearest-rank分位数及实际二进制/engine/model哈希通过。
- P0/P1-pinned/P2-pinned中位74.0857/129.7165/192.6821 images/s；各轮P99中位14.75055/11.548976/14.376522ms。
- 离线固定RGB/同NMS，不是摄像头fps；运行时P1，P2是离线组件。CSV写入计入吞吐、在逐帧latency结束之后。
- 9,024条件样本，CPU固定1.728GHz、GPU1.020GHz；不可读热区明确缺失。CPU/GPU利用率、进程RSS、功耗轨没采，资源对照仍待补。
- evidence/2026-11-16-gpu-formal含20,527,295字节raw ZIP，SHA6610dd0688b9acb968212452e97ec596087eef6b9b2fd86c2d2d3bc6939af55c；本地逐文件、Git index哈希均验。
- 正式source ZIP89文件SHA5d846473fa160c4515549b7e6dc667e375a48ec0943f3c7981aa5b42aa8d78ec。通过hash-object/update-index精确提交这89个输入，PC上的后续M7代码保留未暂存。
- M6功能已验（见m6-ros2.md）；4×20s为初探，正式ROS≥5min×关键3轮仍待补。首次安装加载旧lib的SIGSEGV与修复原日志保留。

## M7源码与已验证边界

- R1源码ZIP117文件SHA dbade280f5e845a81406f055a5a59f0b214e73399295abd8ba7922eb18deb37e。
- R1 Windows CTest13/13（含Python契约10项）；Jetson原生CTest15/15；三个Humble包原生构建通过；旧检测7项真实GPU生命周期回归全部通过。该轮target不是sanitizer。
- R2源码ZIP119文件SHA 2b85768ef6acf9d2427e03a240d062c114d935bc837cbd16729a4d3a758050f7，native/CTest15/15及ROS三包已通过。
  build仍复用build/m7-native-r1和build/install ros2-m7-r1目录名，以源码快照为准。日志分别day/m7-native-build-r2、m7-ros-build-r2。
- R2增加独占GPU worker在全部对象销毁/前序物理完成后释放本进程CUDA context和trim空闲allocator，再启动独立VLM；失败拒绝后续GPU提交。R4实机五个低层操作通过，2次context释放；总体Agent因STATE_CONFLICT仍失败。
- R2 planner使用zh-next-operation-v3中文提示和简明真实DONE上下文；仍由真实模型选择0..8编号，host按注册schema造参数，不做自然语言关键词规则。此提示改变后Python契约10/10通过。
- 相机单owner、同TaskManager。SnapshotStore默认16张/32MiB raw+PNG预算；自有raw副本生成PNG，保存/检测/VQA关联原frame/gen；原图/完整bag/权重/engine不入Git。
- VlmProcess唯一owner，posix_spawn+有界JSONL、MSG_NOSIGNAL、deadline/取消、TERM/KILL/waitpid，实际reap才切回检测；32请求回收profiling累积，闲置10s恢复检测。
- Python agent包agent/edgevision_agent；每步实际ROS Action plan→校验→工具，最多6工具步，队列一活跃一待排、64保留记录；总deadline含等待。报告64目录/128MiB，有界trace、失败回执和真实10s指标窗口。
- tests/agent_tasks_v1.json已固定22任务，模型模式/API负例/transport-fixture分别统计；目前没跑正式整套，不填通过率。3条完整实际轨迹、ROS/Agent竞争、取消/超时、干扰和长稳待验。

## M7实机资产与当前结果（问题正文看journal）

- 自有bridge build/vlm-native-r1/edgevision_vlm_bridge，SHA45c3f30351029b877339172ed19792fa6a9351219d8d402d28d37683969943e9；复用只读旧SDK/static libs/device-link object，构建manifest保存各输入哈希。
- 模型架构Qwen3-VL-2B；base权重SHA7de1838c...977a0与本地HF cache吻合，但缓存engine完整导出谱系未重建，processed_chat_template含domain_lora_v1_merged。不冒称engine一定来自该base权重，不继承旧性能。
- 独立R3实际模型工具选择、COCO图片VQA和非法max_tokens负例通过；R1/R2原始多JSON解析失败保留。
- m7-vlm-assets-r1修改KV/input配置被engine固定profile拒绝，m7-adapter-budget-r4失败保留；不能跳过profile检查。当前使用original profile。
- 新自有m7-vlm-assets-r3排除不需要的可选action目录；旧donor目录不动。独立m7-adapter-no-action-r5通过10.293秒。
- runtime-smoke-r1测试脚本安装路径错，节点未启动；r2 OOM杀ROS节点，shutdown未确认；r3低层snapshot/detect/plan/describe/resume全返回且物理shutdown确认，但Agent重复get_status达到步数上限，总体仍FAILED。
- R3资源采样126项有效，系统MemAvailable最小16,572KiB；资源预算尚未通过，不宣称模型共驻或无检测影响。
- 独立prompt试验day/m7-planner-prompts-r1：中文短提示4/6、英文1/6；不执行工具、不计正式任务成绩，原响应保留。
- 需要真实节点路径：install/ros2-m7-r1/edgevision_ros/lib/edgevision_ros/edgevision_node（colcon隔离安装）。
- helper结果都在父目录.tools/*.json，读取保存JSON，别手抄PTY换行哈希。板无rg。资源/proc/task/children不支持，probe改扫stat PPID，仅保存自有子进程。

## 正在做与下一步

1. 收26078的M7 R2构建/CTest/colcon结果。成功后用tools/m7_runtime_probe.py跑新out=m7-runtime-smoke-r4，node仍ros2-m7-r1路径、assets-r3原profile，无可选Action。
2. functions.store中有m7_smoke_command_r3可取，改out r4即可。R2加入CUDA context释放，必须观察模型切回、capture推进、PSS/可用内存和物理shutdown；不能因编译通过写验收通过。
3. 通过后再做固定任务和3实际轨迹，失败原样留存；22:25前停板端自有负载、保存并通知用户，等其到宿舍再接实机。
4. 整理M7候选源码/证据、README/progress/UPSTREAM、相关领域技能后再精确提交；M7目前全部未提交，不要夹带Windows噪声。
5. 固定journal EV-VLM-003已记录OOM/无效可选expert/规划重复/采样缺项；R4已验2次context释放，最低可用158,412KiB；已补journal。

## 网页与剩余交付

- 展示页PC源码projects/edgevision/site/dist已更新事实，保留工具样式；尚未提交/发布。source de91f03/gh-pages 53402d9已推；在线HTTP200且新进度/数字匹配。
- 已读Sites技能与local preview及delivery skill，已向用户说明使用技能。localhost8769静态服务器PTY52227待停止；HTTP200、HTML片段/资源及JS node --check通过。
- CUA预览启动失败“failed to write kernel assets ... os error3”，已调用唯一completion_summary结束CUA部分；没有浏览器视觉复核，不冒称通过。
- 发布工作树父目录.tools/site-publish-20261116；复制index.html/app.js、精确commit/push gh-pages，再验证在线新内容。不要新建Sites托管，用户明确要Github网页。
- 后续必须补M5资源对照、M6正式矩阵、M7全部任务/干扰，以及M8实时Web/RTSP、systemd/日志/故障恢复、最终30min/2h/12h。静态展示不算实时控制台。

最新下一步覆盖前面的旧施工顺序：R4已跑且Agent失败，别重复做构建。先完成本机M7源码/证据精确提交、更新checkpoint并停PC预览。模型规划重复/错误仍需修复；二元完成校验4/6有过早完成问题，未接产品。不要把这条实验当修复。到宿舍重新在线前不跑板端工作。
