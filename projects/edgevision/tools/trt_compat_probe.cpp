// Hardware regression for the fixed-shape, batch-1 SSD baseline on TensorRT 10.
// Uses the real upstream loader and real GPU execution; no mock backend.
#include <jetson-inference/tensorNet.h>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

class Probe final : public tensorNet {
public:
    Probe() = default;
    bool enqueue() { return ProcessNetwork(false); }
};

int main(int argc, char** argv) {
    if (argc != 3) {
        std::fprintf(stderr, "Usage: trt_compat_probe MODEL.onnx full|auxiliary\n");
        return 2;
    }
    const bool auxiliary = std::string(argv[2]) == "auxiliary";
    if (!auxiliary && std::string(argv[2]) != "full") return 2;
    Probe net;
    const std::vector<std::string> outputs = auxiliary
        ? std::vector<std::string>{"scores"}
        : std::vector<std::string>{"scores", "boxes"};
    if (!net.LoadNetwork(nullptr, argv[1], nullptr, "input_0", outputs, 1, TYPE_FP16)) return 1;
    const auto input = net.GetInputDims();
    const auto scores = net.GetOutputDims();
    if (input.d[0] != 3 || input.d[1] != 300 || input.d[2] != 300 ||
        scores.d[0] != 3000 || scores.d[1] != 9 || scores.d[2] != 1 ||
        net.GetInputSize() != 3 * 300 * 300 * sizeof(float) ||
        net.GetOutputSize() != 3000 * 9 * sizeof(float)) {
        std::fprintf(stderr, "FAIL: SSD shape contract\n");
        return 1;
    }
    if (!auxiliary) {
        const auto boxes = net.GetOutputDims(1);
        if (boxes.d[0] != 3000 || boxes.d[1] != 4 || boxes.d[2] != 1 ||
            net.GetOutputSize(1) != 3000 * 4 * sizeof(float)) return 1;
    }
    if (cudaMemsetAsync(net.GetInputPtr(), 0, net.GetInputSize(), net.GetStream()) != cudaSuccess ||
        !net.enqueue() || cudaStreamSynchronize(net.GetStream()) != cudaSuccess) return 1;
    std::vector<float> scores_cpu(3000 * 9);
    if (cudaMemcpy(scores_cpu.data(), net.GetOutputPtr(), net.GetOutputSize(), cudaMemcpyDeviceToHost) != cudaSuccess) return 1;
    for (float value : scores_cpu) {
        if (!std::isfinite(value) || value < -.001f || value > 1.001f) {
            std::fprintf(stderr, "FAIL: nonfinite or invalid probability\n");
            return 1;
        }
    }
    std::printf("PASS: real GPU enqueue, SSD shapes, finite scores; mode=%s\n", argv[2]);
    return 0;
}
