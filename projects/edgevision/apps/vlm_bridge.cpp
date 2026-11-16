// Local-model adapter. SDK/runtime is reused read-only, never replaced by rules.
#include <edgeqwen/runtime.h>
#include <nlohmann/json.hpp>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
using Json=nlohmann::json;
namespace fs=std::filesystem;
uint64_t mono(){return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();}
bool line(std::string& out){
  out.clear();for(int c;(c=std::getchar())!=EOF;){if(c=='\n')return true;
    if(out.size()>=16384)throw std::runtime_error("VLM_REQUEST_BYTE_BUDGET");out.push_back(char(c));}
  return !out.empty();
}
bool inside(const fs::path& path,const fs::path& root){
  auto a=path.begin(),b=root.begin();for(;b!=root.end();++b,++a)if(a==path.end()||*a!=*b)return false;return true;
}
int main(int argc,char** argv){
  try{
    if(argc!=4)throw std::invalid_argument("usage: edgevision_vlm_bridge LLM_ENGINE_DIR VISUAL_ENGINE_DIR OWNED_IMAGE_ROOT");
    const auto image_root=fs::canonical(argv[3]);
    edgeqwen::Runtime runtime({argv[1],argv[2]});
    // Warmup is optional in the reused SDK. A failed warmup must remain visible.
    const bool warm=runtime.warmup();
    std::cout<<"EDGEVISION_VLM_READY "<<Json({{"schema_version",1},{"warmup_ok",warm},{"model","Qwen3-VL-2B-Instruct"}}).dump()<<std::endl;
    std::string input;
    while(line(input)){
      std::string request_id;
      bool model_called=false;
      const auto started=mono();
      try{
        const auto request=Json::parse(input);
        for(const auto& item:request.items())if(item.key()!="request_id"&&item.key()!="messages"&&item.key()!="max_tokens")throw std::invalid_argument("UNKNOWN_VLM_FIELD");
        request_id=request.at("request_id").get<std::string>();
        const int tokens=request.value("max_tokens",192);
        const auto& messages=request.at("messages");
        if(request_id.empty()||request_id.size()>64||tokens<1||tokens>384||!messages.is_array()||messages.empty()||messages.size()>6)
          throw std::invalid_argument("VLM_REQUEST_BUDGET");
        std::vector<edgeqwen::ChatMessage> parsed;std::size_t text_bytes=0,image_count=0;
        for(const auto& value:messages){
          for(const auto& item:value.items())if(item.key()!="role"&&item.key()!="text"&&item.key()!="image_paths")throw std::invalid_argument("UNKNOWN_MESSAGE_FIELD");
          edgeqwen::ChatMessage message;message.role=value.at("role").get<std::string>();message.text=value.at("text").get<std::string>();
          if(message.role!="system"&&message.role!="user"&&message.role!="assistant")throw std::invalid_argument("VLM_ROLE");
          text_bytes+=message.text.size();if(text_bytes>6000)throw std::invalid_argument("VLM_PROMPT_BUDGET");
          for(const auto& image:value.value("image_paths",Json::array())){
            if(++image_count>1)throw std::invalid_argument("VLM_IMAGE_COUNT");
            const auto path=fs::canonical(image.get<std::string>());
            if(!inside(path,image_root)||!fs::is_regular_file(path)||fs::file_size(path)>4*1024*1024)
              throw std::invalid_argument("VLM_IMAGE_PATH_OR_BUDGET");
            message.imagePaths.push_back(path.string());
          }
          parsed.push_back(std::move(message));
        }
        edgeqwen::GenerationParams params;params.maxGenerateLength=tokens;
        params.temperature=.1f;params.topK=1;params.topP=1;
        model_called=true;
        const auto result=runtime.generate(parsed,params);const auto finished=mono();
        if(result.text.size()>8192)throw std::runtime_error("VLM_OUTPUT_BYTE_BUDGET");
        Json response={{"schema_version",1},{"request_id",request_id},{"success",result.finishReason!="error" && !result.text.empty()},
          {"model","Qwen3-VL-2B-Instruct"},{"text",result.text},{"finish_reason",result.finishReason},
          {"input_tokens",result.inputTokenCount},{"generated_tokens",result.tokenIds.size()},
          {"started_mono_ns",started},{"finished_mono_ns",finished},
          {"profile_reported_by_donor_runtime",Json::parse(runtime.getLastProfile().toJson())}};
        std::cout<<"EDGEVISION_VLM_RESULT "<<response.dump()<<std::endl;
      }catch(const std::exception& e){
        std::cout<<"EDGEVISION_VLM_RESULT "<<Json({{"schema_version",1},{"request_id",request_id},{"success",false},
          {"error_code",std::string(e.what()).substr(0,256)},{"started_mono_ns",started},{"finished_mono_ns",mono()}}).dump()<<std::endl;
        if(model_called)return 70; // runtime failure: do not reuse an uncertain GPU context
      }
    }
    return 0;
  }catch(const std::exception& e){std::cerr<<"EDGEVISION_VLM_FATAL "<<e.what()<<'\n';return 1;}
}
