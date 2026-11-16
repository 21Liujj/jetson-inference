#include "edgevision/yolo_postprocess.h"
#include <cassert>
#include <limits>

int main() {
  std::vector<float> x(84*8400,0.f);
  auto set=[&](int a,int c,float score,float cx=100,float cy=100,float w=40,float h=40) {
    x[a]=cx; x[8400+a]=cy; x[16800+a]=w; x[25200+a]=h; x[(4+c)*8400+a]=score;
  };
  edgevision::LetterboxGeometry g{320,160,0,160,2.f};
  assert(edgevision::decode_yolov8(x.data(),x.size(),g).empty());
  set(0,2,.9,100,200); set(1,2,.8,100,200); set(2,3,.7,100,200);
  auto b=edgevision::decode_yolov8(x.data(),x.size(),g);
  assert(b.size()==2 && b[0].anchor==0 && b[1].class_id==3);
  assert(b[0].left==40 && b[0].top==10 && b[0].right==60 && b[0].bottom==30);
  set(3,4,.25); // Strict > threshold.
  assert(edgevision::decode_yolov8(x.data(),x.size(),g).size()==2);
  set(0,5,.6,100,200);
  assert(edgevision::decode_yolov8(x.data(),x.size(),g,.25,true).size()==3);
  set(4,6,.9,-10,150,100,100);
  b=edgevision::decode_yolov8(x.data(),x.size(),g);
  assert(b[1].anchor==4 && b[1].left==0 && b[1].top==0);
  x[0]=std::numeric_limits<float>::quiet_NaN();
  bool rejected=false;
  try { edgevision::decode_yolov8(x.data(),x.size(),g); } catch(const std::runtime_error&) {rejected=true;}
  assert(rejected);
}
