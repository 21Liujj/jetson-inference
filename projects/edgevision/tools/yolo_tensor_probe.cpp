#include "edgevision/upstream_yolo.h"
#include "edgevision/yolo_postprocess.h"
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <set>
#include <sstream>
#include <stdexcept>

namespace fs=std::filesystem;
void boxes_json(std::ostream& out,const std::vector<edgevision::YoloBox>& boxes) {
  out << '[';
  for(std::size_t i=0;i<boxes.size();++i) {
    const auto& b=boxes[i]; if(i) out << ',';
    out << '[' << b.anchor << ',' << b.class_id << ',' << b.score << ',' << b.left << ',' << b.top << ',' << b.right << ',' << b.bottom << ']';
  }
  out << ']';
}
int main(int argc,char** argv) {
  try {
    if(argc!=4 && argc!=5) throw std::invalid_argument("usage: edgevision_yolo_tensor_probe MODEL.onnx CORPUS_DIR NEW_OUTPUT_DIR [fp32]");
    const fs::path corpus=fs::canonical(argv[2]), out=fs::absolute(argv[3]);
    if(!fs::create_directory(out)) throw std::runtime_error("OUTPUT_MUST_BE_NEW");
    const bool fp16=argc!=5;
    if(!fp16 && std::string(argv[4])!="fp32") throw std::invalid_argument("INVALID_PRECISION");
    edgevision::UpstreamYolo net; net.initialize(argv[1],fp16);
    std::ifstream list(corpus/"inputs.tsv");
    std::ofstream results(out/"detections.jsonl"); results << std::setprecision(9);
    if(!list || !results) throw std::runtime_error("CORPUS_OR_OUTPUT_OPEN_FAILED");
    std::set<int> ids; std::string line; int total=0;
    while(std::getline(list,line)) {
      std::istringstream row(line); int id,fixed; edgevision::LetterboxGeometry g{};
      if(!(row >> id >> g.width >> g.height >> g.left >> g.top >> g.scale >> fixed) || id<0 || !ids.insert(id).second)
        throw std::runtime_error("INVALID_CORPUS_ROW");
      std::string extra; if(row >> extra) throw std::runtime_error("EXTRA_CORPUS_FIELDS");
      const auto path=corpus/(std::to_string(id)+".u8");
      if(fs::file_size(path)!=3*640*640) throw std::runtime_error("TENSOR_SIZE_MISMATCH");
      std::vector<unsigned char> bytes(3*640*640); std::ifstream input(path,std::ios::binary);
      if(!input.read(reinterpret_cast<char*>(bytes.data()),bytes.size())) throw std::runtime_error("TENSOR_READ_FAILED");
      std::vector<float> x(bytes.size());
      for(std::size_t i=0;i<x.size();++i) x[i]=float(bytes[i])/255.f;
      const auto y=net.infer(x.data(),x.size());
      const auto display=edgevision::decode_yolov8(y.data(),y.size(),g,.25f,false);
      const auto eval=edgevision::decode_yolov8(y.data(),y.size(),g,.001f,true);
      if(fixed) {
        std::ofstream raw(out/(std::to_string(id)+".f32"),std::ios::binary);
        if(!raw.write(reinterpret_cast<const char*>(y.data()),y.size()*sizeof(float))) throw std::runtime_error("RAW_OUTPUT_WRITE_FAILED");
      }
      results << "{\"image_id\":" << id << ",\"display\":"; boxes_json(results,display);
      results << ",\"evaluation\":"; boxes_json(results,eval); results << "}\n"; results.flush();
      if(!results) throw std::runtime_error("RESULT_WRITE_FAILED");
      std::cout << "completed " << id << " display=" << display.size() << " evaluation=" << eval.size() << '\n';
      ++total;
    }
    if(total<20 || !list.eof()) throw std::runtime_error("INCOMPLETE_CORPUS");
    std::ofstream done(out/"summary.json");
    done << "{\"images\":" << total << ",\"precision\":\"" << (fp16?"FP16":"FP32") << "\",\"normal_exit\":true}\n";
    std::cout << "YOLO_TENSOR_PROBE_PASS images=" << total << '\n';
    return 0;
  } catch(const std::exception& e) { std::cerr << "YOLO_TENSOR_PROBE_FAILED: " << e.what() << '\n'; return 1; }
}
