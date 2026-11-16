// Acceptance injection ONLY. One CUDA thread keeps real device work pending so
// a cancellation/deadline test cannot pass by canceling a job before launch.
// This is not a preprocessing kernel, throughput benchmark or product feature.
#include <cuda_runtime.h>
__global__ void acceptance_delay(unsigned long long cycles) {
  const auto start = clock64();
  while (clock64() - start < cycles) {}
}
extern "C" cudaError_t edgevision_acceptance_delay(unsigned milliseconds) {
  if (!milliseconds || milliseconds>200) return cudaErrorInvalidValue;
  int device=0;auto e=cudaGetDevice(&device);if(e!=cudaSuccess)return e;
  cudaDeviceProp prop{};e=cudaGetDeviceProperties(&prop,device);if(e!=cudaSuccess)return e;
  acceptance_delay<<<1,1,0,0>>>(static_cast<unsigned long long>(prop.clockRate)*milliseconds);
  return cudaGetLastError();
}
