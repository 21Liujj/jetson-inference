#pragma once
#ifdef EDGEVISION_NVTX
#include <nvtx3/nvToolsExt.h>
#endif
namespace edgevision {
class NvtxScope {
 public:
  explicit NvtxScope(const char* name) {
#ifdef EDGEVISION_NVTX
    nvtxRangePushA(name);
#else
    (void)name;
#endif
  }
  ~NvtxScope() {
#ifdef EDGEVISION_NVTX
    nvtxRangePop();
#endif
  }
  NvtxScope(const NvtxScope&)=delete;
  NvtxScope& operator=(const NvtxScope&)=delete;
};
}
