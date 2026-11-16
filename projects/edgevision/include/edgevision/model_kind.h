#pragma once
namespace edgevision {
enum class DetectorKind { SsdFruit, YoloV8Coco };
enum class PreprocessMode { CpuBaseline, CudaFused };
} // namespace edgevision
