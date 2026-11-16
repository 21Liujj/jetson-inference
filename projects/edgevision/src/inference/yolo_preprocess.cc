#include "edgevision/yolo_preprocess.h"
#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>
#include <limits>

namespace edgevision {
namespace {
int round_even(double value) {
  const auto base=std::floor(value), fraction=value-base;
  return int(base)+(fraction>.5 || (fraction==.5 && int(base)%2));
}
}
LetterboxGeometry yolo_letterbox_rgb(const uint8_t* rgb,std::size_t bytes,unsigned w,unsigned h,
                                    std::size_t stride,std::vector<uint8_t>& chw) {
  if(!rgb || !w || !h || w>8192 || h>8192 || uint64_t(w)*h>32*1024*1024 ||
     stride<std::size_t(w)*3 || stride>std::numeric_limits<std::size_t>::max()/h ||
     bytes<(h-1)*stride+std::size_t(w)*3) throw std::invalid_argument("INVALID_RGB_LAYOUT");
  const double scale=std::min(640./w,640./h);
  const int rw=round_even(w*scale),rh=round_even(h*scale);
  if(rw<1 || rh<1 || rw>640 || rh>640) throw std::invalid_argument("UNSUPPORTED_RGB_ASPECT_RATIO");
  const int left=(640-rw)/2,top=(640-rh)/2;
  cv::Mat input(h,w,CV_8UC3,const_cast<uint8_t*>(rgb),stride), resized, padded;
  cv::resize(input,resized,cv::Size(rw,rh),0,0,cv::INTER_LINEAR);
  cv::copyMakeBorder(resized,padded,top,640-rh-top,left,640-rw-left,cv::BORDER_CONSTANT,cv::Scalar(114,114,114));
  chw.resize(3*640*640);
  for(int y=0;y<640;++y) {
    const auto* row=padded.ptr<uint8_t>(y);
    for(int x=0;x<640;++x) for(int c=0;c<3;++c) chw[c*640*640+y*640+x]=row[x*3+c];
  }
  return {int(w),int(h),left,top,float(scale)};
}
} // namespace edgevision
