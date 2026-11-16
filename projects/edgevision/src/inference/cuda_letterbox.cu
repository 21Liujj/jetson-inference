#include "edgevision/cuda_letterbox.h"
#include <cuda_runtime.h>
#include <limits>

namespace edgevision {
namespace {
int round_even(double value) {
  const auto base=std::floor(value), fraction=value-base;
  return int(base)+(fraction>.5 || (fraction==.5 && int(base)%2));
}
__global__ void rgb_letterbox_kernel(const uint8_t* rgb,unsigned w,unsigned h,std::size_t stride,
                                    int rw,int rh,int left,int top,float* out) {
  const unsigned index=blockIdx.x*blockDim.x+threadIdx.x;
  if(index>=640*640) return;
  const int x=int(index%640)-left,y=int(index/640)-top;
  if(x<0 || y<0 || x>=rw || y>=rh) {
    for(int c=0;c<3;++c) out[c*640*640+index]=114.f/255.f;
    return;
  }
  const float sx=(float(x)+.5f)*(float(w)/rw)-.5f;
  const float sy=(float(y)+.5f)*(float(h)/rh)-.5f;
  int x0=__float2int_rd(sx),y0=__float2int_rd(sy);
  float fx=sx-x0,fy=sy-y0;
  if(x0<0) {x0=0;fx=0;} if(x0>=int(w)-1) {x0=w-1;fx=0;}
  if(y0<0) {y0=0;fy=0;} if(y0>=int(h)-1) {y0=h-1;fy=0;}
  const int x1=min(x0+1,int(w)-1),y1=min(y0+1,int(h)-1);
  for(int c=0;c<3;++c) {
    const float a=rgb[std::size_t(y0)*stride+x0*3+c],b=rgb[std::size_t(y0)*stride+x1*3+c];
    const float d=rgb[std::size_t(y1)*stride+x0*3+c],e=rgb[std::size_t(y1)*stride+x1*3+c];
    const float value=(a+(b-a)*fx)+((d+(e-d)*fx)-(a+(b-a)*fx))*fy;
    const int quantized=max(0,min(255,__float2int_rn(value)));
    out[c*640*640+index]=float(quantized)/255.f;
  }
}
}
CudaLetterboxPlan cuda_letterbox_plan(unsigned w,unsigned h) {
  if(!w || !h || w>8192 || h>8192 || uint64_t(w)*h>32*1024*1024)
    throw std::invalid_argument("INVALID_RGB_GEOMETRY");
  const double scale=std::min(640./w,640./h);
  const int rw=round_even(w*scale),rh=round_even(h*scale);
  if(rw<1 || rh<1 || rw>640 || rh>640) throw std::invalid_argument("UNSUPPORTED_RGB_ASPECT_RATIO");
  return {{int(w),int(h),(640-rw)/2,(640-rh)/2,float(scale)},rw,rh};
}
cudaError_t launch_rgb_letterbox(const uint8_t* rgb,std::size_t bytes,unsigned w,unsigned h,
                                 std::size_t stride,float* chw,std::size_t elements,cudaStream_t stream) {
  if(!rgb || !chw || elements!=3*640*640 || !w || !h || stride<std::size_t(w)*3 ||
     stride>std::numeric_limits<std::size_t>::max()/h || bytes<(h-1)*stride+std::size_t(w)*3)
    return cudaErrorInvalidValue;
  CudaLetterboxPlan plan;
  try { plan=cuda_letterbox_plan(w,h); } catch(const std::invalid_argument&) {return cudaErrorInvalidValue;}
  rgb_letterbox_kernel<<<(640*640+255)/256,256,0,stream>>>(rgb,w,h,stride,plan.resized_width,
    plan.resized_height,plan.geometry.left,plan.geometry.top,chw);
  return cudaGetLastError();
}
} // namespace edgevision
