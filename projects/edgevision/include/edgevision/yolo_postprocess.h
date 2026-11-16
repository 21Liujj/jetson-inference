#pragma once
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <stdexcept>
#include <vector>

namespace edgevision {
// Static YOLOv8 detection contract only. No TRT/ROS/OpenCV dependency.
struct YoloBox {
  int anchor = 0, class_id = 0;
  float score = 0, left = 0, top = 0, right = 0, bottom = 0;
};
struct LetterboxGeometry {
  int width, height, left, top;
  float scale;
};
inline float box_iou(const YoloBox& a, const YoloBox& b) {
  const float intersection = std::max(0.f, std::min(a.right,b.right)-std::max(a.left,b.left)) *
                             std::max(0.f, std::min(a.bottom,b.bottom)-std::max(a.top,b.top));
  const float total = (a.right-a.left)*(a.bottom-a.top)+(b.right-b.left)*(b.bottom-b.top)-intersection;
  return total > 0 ? intersection / total : 0.f;
}
inline std::vector<YoloBox> decode_yolov8(const float* values, std::size_t count,
                                        LetterboxGeometry g, float confidence=.25f,
                                        bool multi_label=false, float nms=.7f) {
  constexpr int anchors=8400, classes=80;
  if (!values || count != 84*anchors || g.width <= 0 || g.height <= 0 ||
      g.left < 0 || g.top < 0 || !std::isfinite(g.scale) || g.scale <= 0 ||
      !std::isfinite(confidence) || !std::isfinite(nms) ||
      confidence < 0 || confidence > 1 || nms < 0 || nms > 1)
    throw std::invalid_argument("INVALID_YOLO_CONTRACT");
  for (std::size_t i=0; i<count; ++i)
    if (!std::isfinite(values[i])) throw std::runtime_error("NONFINITE_YOLO_OUTPUT");
  std::vector<YoloBox> candidates;
  for (int a=0; a<anchors; ++a) {
    const float cx=values[a], cy=values[anchors+a], w=values[2*anchors+a], h=values[3*anchors+a];
    if (w < 0 || h < 0) throw std::runtime_error("NEGATIVE_YOLO_BOX");
    int best=0;
    for (int c=1; c<classes; ++c) if (values[(4+c)*anchors+a] > values[(4+best)*anchors+a]) best=c;
    for (int c=0; c<classes; ++c) {
      const float score=values[(4+c)*anchors+a];
      if (score < 0 || score > 1) throw std::runtime_error("INVALID_YOLO_PROBABILITY");
      if ((!multi_label && c != best) || score <= confidence) continue;
      candidates.push_back({a,c,score,cx-w/2,cy-h/2,cx+w/2,cy+h/2});
    }
  }
  std::sort(candidates.begin(), candidates.end(), [](const YoloBox& a,const YoloBox& b) {
    if (a.score != b.score) return a.score > b.score;
    return a.anchor != b.anchor ? a.anchor < b.anchor : a.class_id < b.class_id;
  });
  if (candidates.size() > 30000) candidates.resize(30000);
  std::vector<YoloBox> selected;
  for (auto b : candidates) {
    bool suppress=false;
    for (const auto& previous : selected)
      if (b.class_id == previous.class_id && box_iou(b,previous)>nms) { suppress=true; break; }
    if (suppress) continue;
    selected.push_back(b);
    if (selected.size() == 300) break;
  }
  // NMS is performed BEFORE clipping and coordinate inversion, matching the
  // reference model contract. Border padding must not change suppression.
  for (auto& b : selected) {
    b.left=std::clamp((b.left-g.left)/g.scale,0.f,float(g.width));
    b.right=std::clamp((b.right-g.left)/g.scale,0.f,float(g.width));
    b.top=std::clamp((b.top-g.top)/g.scale,0.f,float(g.height));
    b.bottom=std::clamp((b.bottom-g.top)/g.scale,0.f,float(g.height));
  }
  return selected;
}
} // namespace edgevision
