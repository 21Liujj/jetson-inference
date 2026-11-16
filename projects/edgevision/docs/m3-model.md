# M3 · YOLOv8n 部署与数值基线

2026-11-16 验收通过。复用个人旧工程中的模型文件，个人适配器继承修复后的上游
`tensorNet`，没有换成独立TensorRT框架。旧engine不作本轮证据；在目标Orin Nano上
从冻结ONNX首次构建，再分别验证固定输入、C++预处理、真实视频与统一任务生命周期。
[冻结契约](../models/yolov8n-coco.json) · [证据](../evidence/2026-11-16-yolo/summary.json)。

## 模型与参考

- ONNX SHA256：`e4b6513441dd6ba0976b5ab2b9702811f5a2265342baa0aca0c082cffdea7940`。
- 原文件metadata：Ultralytics8.3.159、PyTorch2.5.0、opset12、静态batch1、simplify=true、
  half=false、nms=false。原始下载/导出日志没有保留；这里是审计现有artifact，未冒称重新导出。
- 输入`images`：FP32 RGB NCHW `1×3×640×640`，uint8/255；输出`output0`：
  FP32 `1×84×8400`，4个cxcywh坐标通道加80个类别概率，没有objectness通道。
- letterbox：OpenCV INTER_LINEAR、保持比例、允许放大、居中填114；实际整数padding
  用于逆映射。NMS按类、IoU0.7，显示阈值0.25；精度评估阈值0.001、multi-label、最多300框。
- 类别表来自ONNX metadata，与COCO的80个类别名逐项相符；COCO category ID不等于class+1。
- PC隔离环境：numpy1.26.4、ONNX1.17.0、ORT1.20.1 CPUExecutionProvider、OpenCV4.10.0、
  pycocotools2.0.10；没有升级Jetson CUDA/JetPack。

## 首轮前冻结的门槛与结果

从已有COCO val2017副本按标注密度、横竖比例、尺寸和边界目标先选20张，再按image ID
补至200张；选样不看模型预测。另加4张不同尺寸、纯色/空画面夹具，独立于200张带标注精度集。
固定RGB、CHW uint8及归一化float的哈希，目标逐项核对。

| 验证 | 结果 |
|---|---|
| 独立C++ vs NumPy解析 | 24张固定图的显示/评估解析无失败 |
| C++ CPU letterbox vs PC | 24张CHW逐字节相同；额外17字节行padding与截断拒绝通过 |
| ORT参考 / TensorRT FP16 AP | 0.394822295 / 0.394353343 |
| AP绝对下降 | 0.000468952，即0.0468952个百分点；首轮门槛0.5个百分点 |
| 固定图高置信框匹配 | 199/200，99.5%；首轮门槛98%，匹配IoU≥0.95、分差≤0.05 |
| R1与最终R4的目标输出 | 全204张解析JSON和24张原始tensor逐字节一致 |
| Windows / Jetson sanitizer | 10/10；ASan/UBSan/LSan 12/12，72.24s |

AP是该选定子集的COCOeval bbox AP@[.50:.05:.95]、maxDets100，不是全COCO发布精度。
坐标和分数原始误差分开统计；不把接近0分数的相对误差当统一门槛。
唯一未匹配框是图470924、anchor1642、class56：0.252635→0.249390，跨过0.25显示阈值。
原始输出与该失败样例保留，没有降低阈值掩盖变化。

## 目标部署与真实设备

本轮新engine SHA256：`2925e839f38ff81f186db6284e6e369c23399fdd50914b10526fd8a8c0474519`。
TensorRT10.3、CUDA12.6、Orin Nano；FP16指builder允许半精度，实际可含FP32/TF32算子，
不是所有算子强制半精度。I/O保持FP32；workspace日志请求512MiB，上游TRT10没有显式
setMemoryPoolLimit，实际默认上限未测，不宣称已限制为512MiB。

60秒真实USB视频窗口完成1,803次推理，发布1,802次；停止竞态中1个结果失效，最终pool0，
正常退出。该画面全部有效空检测，不作为相机精度或正式FPS/P99基准。
首轮统一任务测试R4在重启generation断言失败；生产路径已有合法启动重试，原失败保留。
修正测试为核对`新generation-旧generation = 1+start_retries`并记录重试，未修改重连策略。
R5 YOLO与原SSD各7项真实GPU生命周期测试均通过，包括过期、取消、stop、重启及shutdown。
显式200ms GPU负载只用于验收，默认构建关闭，不是模型优化。

## 代码与复现入口

- `src/inference/upstream_yolo.cc`：上游tensorNet的静态模型适配，单owner/context。
- `yolo_preprocess.cc`与`yolo_postprocess.h`：RGB布局校验、CPU基线、独立NMS和坐标回映。
- `UpstreamDetector`/`UpstreamExecutor`显式选择`YoloV8Coco`；原SSD默认保留，仍共用一个TaskManager。
- `yolo_reference.py prepare|reference|compare`：冻结样本、PC参考、独立解析及同样本COCOeval。
- `edgevision_yolo_tensor_probe`、`edgevision_yolo_preprocess_probe`、`edgevision_inference_probe --kind yolov8`、
  `edgevision_executor_probe --kind yolov8`：目标数值/像素/视频/生命周期验证。

示例命令与全部实际参数在原始证据的监督run.json中。参考依赖见
`tools/requirements-model-reference.txt`。模型/engine/完整数值输入输出留在artifact目录，
Git保存哈希、来源、代码快照、解析结果和原始文本记录；不分发权重或私人相机照片。
模型metadata标明AGPL-3.0，现有权重的来源限制一并记录。

本阶段仍为串行CPU letterbox基线，GPU→CPU→GPU往返是M5实际待优化路径，
没有把本阶段写成已经实现CUDA融合、双缓冲重叠或正式性能提升。
问题正文见journal EV-GPU-003、EV-TEST-002。
