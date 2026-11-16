#pragma once
#include "edgevision/frame.h"
#include <cstring>
#include <stdexcept>

namespace edgevision {
// Bounded adapter contract. This packs rows, not colors or normalization.
inline std::size_t packed_yuyv_bytes(const FrameMeta& m, std::size_t available) {
  constexpr uint32_t yuyv = uint32_t('Y') | (uint32_t('U') << 8) | (uint32_t('Y') << 16) | (uint32_t('V') << 24);
  if (m.pixfmt_fourcc != yuyv || !m.width || !m.height || m.width > 1920 || m.height > 1080 || m.width % 2)
    throw std::invalid_argument("UNSUPPORTED_YUYV_GEOMETRY");
  const uint64_t row = uint64_t(m.width) * 2;
  const uint64_t needed = (uint64_t(m.height) - 1) * m.stride + row;
  if (m.stride < row || m.bytesused < needed || m.bytesused > available)
    throw std::invalid_argument("INVALID_YUYV_LAYOUT");
  return std::size_t(row * m.height);
}
inline void pack_yuyv(const Frame& f, void* destination, std::size_t capacity) {
  const auto n = packed_yuyv_bytes(f, f.data.size());
  if (!destination || capacity < n) throw std::invalid_argument("YUYV_DESTINATION_BUDGET");
  const auto row = std::size_t(f.width) * 2;
  for (uint32_t y = 0; y < f.height; ++y)
    std::memcpy(static_cast<uint8_t*>(destination) + y * row, f.data.data() + std::size_t(y) * f.stride, row);
}
} // namespace edgevision
