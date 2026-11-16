# M4 · UVC 帧完成追踪与用户态关联

2026-11-16 实机验收通过。补丁针对已验证的 L4T R36.4.3 / 5.15.148-tegra UVC
源码，新增帧完成/内部回收及 stream 生命周期事件；原版、关闭追踪、开启追踪、
真实 USB 拔插和实际回退均已执行。证据：[汇总](../evidence/2026-11-16-uvc-trace/summary.json)、
[清单](../evidence/2026-11-16-uvc-trace/manifest.json)、
[原始 trace、CSV、构建和回退记录](../evidence/2026-11-16-uvc-trace/raw-uvc-trace-evidence.zip)。

## 新增了什么

先采集原版已有 `v4l2_qbuf`、`v4l2_dqbuf`、`vb2_v4l2_buf_done`、`vb2_buf_done`。
这些事件已有 sequence、部分长度/队列信息，不能把它们写成个人新增功能。
缺口是 UVC 私有 error/state、内部损坏帧重排原因，以及跨 STREAMON 的明确 stream ID。
内部回收根本没有交到 vb2，单看用户态 DQBUF 无法直接观察它。

- [补丁](../kernel/0001-uvc-frame-observability.patch)：修改4个文件，保持原 GPL 归属。
- `edgevision_uvc:uvc_frame`：USB物理路径、interface、video minor、stream ID、
  index、sequence、bytesused、UVC state/error、reason、`ktime_get_ns()`。
- reason 0：最后异步复制引用释放后交给 vb2；1：损坏帧内部回收；2：取消/停止归还。
  正常一帧一个事件，没有每包/每 URB printk。
- `edgevision_uvc:uvc_stream`：启动尝试、启动失败、停止、断连取消。
  stream ID 在提交 URB 前分配；整个模块加载期间唯一，不假定等于用户态 generation。
- 最后引用释放处已有 buffer/queue 生命周期保障。只在 enabled 时读取和组装事件，
  不增加队列遍历或锁序，不在 USB/异步完成路径睡眠；metadata/output queue 不套用
  capture queue 的 container_of。

## 真实诊断样例

开启追踪、100ms 慢消费者、8秒窗口：

| 层次 | 实测 |
|---|---:|
| 交给 vb2 的帧 / 自有池复制 | 241 / 241 |
| 消费者实际读到 | 80 |
| READY旧帧替换 / 停止清队列 | 159 / 2 |
| 自有池无槽丢弃 / driver sequence缺口 | 0 / 0 |
| 额外的 UVC 内部损坏帧回收 | 1 |

`241 = 80 + 159 + 2`：主要差额来自用户态保持新鲜度的队列策略。
消费的80帧全部关联；完成到DQ的P95为22.209µs，发布到消费P95为61.274ms。
这些是该短窗的观测值，不是最终系统的性能指标。

原始 trace 另显示 `stream_id=11 sequence=0 bytesused=668 state=3 error=1 reason=1`，
协商 YUYV 帧容量为460800字节，该帧被 UVC 内部回收，没有对应 DQBUF。
这是新增功能直接观察到的私有驱动分支；不能把它混进159次用户态 READY 替换。
恢复测试也观察到一次 `bytesused=460800 error=1` 的内部回收，因此长度正常并不表示
UVC error 一定为0。当前未采逐包原因，未声称修复驱动/固件缺陷。

## 正常、启停和拔插

R1完成三种模块状态的功能矩阵；当时后台存在M3 engine构建，时序不能作独立开销结论。
R2在构建结束后重跑：每种状态3×10秒正常、8秒慢消费、3×2秒启停。
开启追踪的正常和慢消费/启停记录均完整关联，所有 trace loss 计数为0。

| R2 状态 | 每个10秒窗口消费帧 | 监督子进程CPU占单核比例中位数 | DQ→消费P95的三轮中位数 |
|---|---|---:|---:|
| 系统原模块 | 300 / 301 / 301 | 1.1295% | 0.954308ms |
| 补丁加载、追踪关闭 | 301 / 301 / 300 | 1.1426% | 0.954980ms |
| 补丁加载、追踪开启 | 301 / 300 / 300 | 1.1283% | 0.947843ms |

CPU口径包含 capture probe 及监督进程，不等于完整内核追踪成本。原始/proc/stat亦保存。
本轮未单独记录并冻结功耗/频率条件；这些结果未分辨出稳定差异，不支持“零开销”或精确开销上界。

用户真实拔插后约 **7.3799s** 收到新会话有效帧，继续交付2秒再停止。
1,803条消费记录全部关联；内核 stream ID `1→2`，用户 generation `1→4`，
中间包括3次有限重开。测试策略为5次/30秒，不推广为默认3次/15秒结论。

## 构建、核验与回退

补丁 SHA256 `32e5bbc7f387ec3e571bff48e61edf9821216473f0fff822225b603a1f81595d`；
172个导入符号CRC全部匹配，headers配置/符号表未改变。
候选模块 GNU Build ID `f090bf2fb690eb3630414267c6d571ac69802b37`；
每次实验 finally 卸载候选并 modprobe 系统模块，再实际采集5秒。
最终 sysfs Build ID 恢复为 `4f3253a8a815a048b37be4908a51db4ef5b66053`。
没有覆盖/lib/modules、depmod、启动项或签名策略；taint保持12288，见
[原版准备与边界](m3-kernel.md)。

入口依次为 `make_uvc_trace_patch.py`、`build_uvc_trace.py`、
`verify_uvc_trace.py --phase prepare|matrix|recovery|restore`。
每次使用新的artifact目录；prepare核对文件哈希、备份及视频owner，实际参数保存在plan。
`uvc_trace_capture.py`使用独立tracefs instance、mono时钟、有界buffer，并在结束移除instance，
不重置其他追踪会话。`parse_uvc_trace.py`关联CSV，拒绝歧义、反向时间和trace丢失。
两项解析测试覆盖sequence回绕、不同stream复用sequence、歧义及内部回收不误配。

问题唯一正文见 [journal](debugging_journal.md) 的 EV-KERNEL-001/002、EV-EVIDENCE-002。
