#include "edgevision/yolo_preprocess.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>

namespace fs=std::filesystem;
int main(int argc,char** argv) {
  try {
    if(argc!=3) throw std::invalid_argument("usage: edgevision_yolo_preprocess_probe CORPUS NEW_OUT");
    const fs::path corpus=argv[1],out=argv[2];
    if(!fs::create_directory(out)) throw std::runtime_error("OUTPUT_MUST_BE_NEW");
    std::ifstream list(corpus/"inputs.tsv"); std::ofstream log(out/"pixels.jsonl");
    if(!list || !log) throw std::runtime_error("CORPUS_OR_OUTPUT_OPEN_FAILED");
    std::string line; unsigned total=0; bool pass=true;
    while(std::getline(list,line)) {
      std::istringstream row(line); int id,w,h,left,top,fixed; float scale;
      if(!(row>>id>>w>>h>>left>>top>>scale>>fixed)) throw std::runtime_error("INVALID_CORPUS_ROW");
      if(!fixed) continue;
      if(id<0 || w<1 || h<1 || w>8192 || h>8192) throw std::runtime_error("INVALID_CORPUS_GEOMETRY");
      std::vector<uint8_t> rgb(std::size_t(w)*h*3),reference(3*640*640),actual;
      std::ifstream image(corpus/(std::to_string(id)+".rgb"),std::ios::binary), expected(corpus/(std::to_string(id)+".u8"),std::ios::binary);
      if(!image.read(reinterpret_cast<char*>(rgb.data()),rgb.size()) || !expected.read(reinterpret_cast<char*>(reference.data()),reference.size()))
        throw std::runtime_error("FIXED_INPUT_READ_FAILED");
      // Deliberately pad rows with sentinel values: stride must be honored.
      std::vector<uint8_t> strided(std::size_t(w*3+17)*h,213);
      for(int y=0;y<h;++y) std::copy_n(rgb.data()+std::size_t(y)*w*3,w*3,strided.data()+std::size_t(y)*(w*3+17));
      const auto geometry=edgevision::yolo_letterbox_rgb(strided.data(),strided.size(),w,h,w*3+17,actual);
      unsigned maximum=0; uint64_t sum=0,unequal=0;
      for(std::size_t i=0;i<actual.size();++i) {const unsigned d=std::abs(int(actual[i])-int(reference[i]));maximum=std::max(maximum,d);sum+=d;unequal+=d!=0;}
      const double mean=double(sum)/actual.size();
      const bool okay=maximum<=1 && mean<=.1 && geometry.left==left && geometry.top==top && std::abs(geometry.scale-scale)<1e-6;
      pass &= okay;
      log << "{\"image_id\":" << id << ",\"max_abs_u8\":" << maximum << ",\"mean_abs_u8\":" << mean
          << ",\"unequal\":" << unequal << ",\"pass\":" << (okay?"true":"false") << "}\n";
      std::ofstream saved(out/(std::to_string(id)+".u8"),std::ios::binary);
      if(!saved.write(reinterpret_cast<const char*>(actual.data()),actual.size())) throw std::runtime_error("OUTPUT_WRITE_FAILED");
      ++total;
    }
    bool rejected=false; std::vector<uint8_t> dummy;
    try { edgevision::yolo_letterbox_rgb(reinterpret_cast<const uint8_t*>("abc"),3,100,100,300,dummy); }
    catch(const std::invalid_argument&) { rejected=true; }
    pass &= rejected && total>=20;
    std::ofstream summary(out/"summary.json"); summary << "{\"fixed_images\":" << total << ",\"truncation_rejected\":" << (rejected?"true":"false")
      << ",\"pass\":" << (pass?"true":"false") << "}\n";
    std::cout << "CPU_LETTERBOX_REGRESSION " << (pass?"PASS":"FAIL") << " images=" << total << '\n';
    return pass?0:1;
  } catch(const std::exception& e) {std::cerr << "CPU_LETTERBOX_FAILED: " << e.what() << '\n';return 1;}
}
