#pragma once
#include "edgevision/yolo_postprocess.h"
#include <cstdint>

namespace edgevision {
// Serial CPU baseline for decoded RGB8 (including padded source rows).
// Pixel regression gate, declared before first target comparison:
// identical geometry/channel order; max abs <=1 uint8, mean abs <=0.1 uint8.
LetterboxGeometry yolo_letterbox_rgb(const uint8_t* rgb, std::size_t bytes,
                                    unsigned width, unsigned height, std::size_t stride,
                                    std::vector<uint8_t>& chw);
} // namespace edgevision
