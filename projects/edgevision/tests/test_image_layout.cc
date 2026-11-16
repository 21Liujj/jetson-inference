#include "edgevision/image_layout.h"
#include <cassert>
#include <cstdio>
using namespace edgevision;
int main() {
  Frame f; f.width = 2; f.height = 2; f.stride = 8; f.bytesused = 12;
  f.pixfmt_fourcc = uint32_t('Y') | (uint32_t('U')<<8) | (uint32_t('Y')<<16) | (uint32_t('V')<<24);
  f.data = {1,2,3,4,90,91,92,93,5,6,7,8};
  uint8_t output[10] = {99,99,99,99,99,99,99,99,99,99};
  pack_yuyv(f, output+1, 8);
  assert(output[0] == 99 && output[9] == 99);
  for (unsigned i=1;i<=8;++i) assert(output[i]==i); // padding must never become pixels
  auto rejects = [&](const Frame& frame, std::size_t n) {
    try { pack_yuyv(frame, output, n); return false; } catch (const std::invalid_argument&) { return true; }
  };
  assert(rejects(f,7)); f.bytesused=11; assert(rejects(f,10));
  f.bytesused=13; assert(rejects(f,10)); f.bytesused=12; f.width=3; assert(rejects(f,10));
  f.width=2; f.stride=3; assert(rejects(f,10)); f.stride=8; f.height=0; assert(rejects(f,10));
  f.height=2; f.pixfmt_fourcc=0; assert(rejects(f,10));
  std::puts("PASS YUYV row padding, guard bytes, truncation, stride, dimensions and format; CPU only");
}
