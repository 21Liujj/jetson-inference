#include "edgevision/agent_tools.h"
#include "edgevision/image_layout.h"
#include <nlohmann/json.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>
#include <chrono>
#include <cctype>
#include <algorithm>

namespace edgevision {
namespace {
using Json=nlohmann::json;
uint64_t mono(){return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();}
void encode(const Frame& frame,const std::filesystem::path& path){
  (void)packed_yuyv_bytes(frame,frame.data.size());
  cv::Mat packed(frame.height,frame.width,CV_8UC2,const_cast<uint8_t*>(frame.data.data()),frame.stride),bgr;
  cv::cvtColor(packed,bgr,cv::COLOR_YUV2BGR_YUY2);
  if(!cv::imwrite(path.string(),bgr,{cv::IMWRITE_PNG_COMPRESSION,3}))throw std::runtime_error("SNAPSHOT_ENCODE_FAILED");
}
const char* tools[]={"get_status","start_capture","stop_capture","capture_snapshot","detect_snapshot","describe_snapshot","get_recent_metrics","create_report","finish"};
const char* plan_system=
  "你是机器人视觉任务规划器。只回复一个数字，不要解释。工具编号：0=查询设备状态get_status，1=启动采集start_capture，2=停止采集stop_capture，3=拍照capture_snapshot，4=检测照片detect_snapshot，5=描述照片describe_snapshot，6=读取最近指标get_recent_metrics，7=生成报告create_report，8=结束finish。根据用户目标和已经完成的真实操作，选择下一项尚未完成且必要的工具。相机正在运行就不再启动。检测和描述之前需要照片。绝不重复已经完成的工作。用户目标已全部完成时必须回复8。";
}
AgentTools::AgentTools(const AgentBackendConfig& config)
  :snapshots_(config.snapshot_root,encode,config.max_snapshots){
    if(!config.vlm_binary.empty())model_=std::make_unique<VlmProcess>(config);
  }
SnapshotRecord AgentTools::save(const Frame& frame,uint64_t task,uint64_t at){return snapshots_.save(frame,task,at);}
FrameLease AgentTools::replay(const std::string& id,uint64_t generation,uint64_t at)const{return snapshots_.replay(id,generation,at);}
ExecutionCompletion AgentTools::snapshot_result(const ExecutionJob& job,const Frame& frame){
  const auto at=mono();const auto saved=save(frame,job.task_id,at);
  Json result={{"schema_version",1},{"task_id",job.task_id},{"generation",saved.frame.session_generation},
    {"frame_id",saved.frame.frame_id},{"snapshot_id",saved.id},{"path",saved.path},{"encoding","png"},
    {"encoded_bytes",saved.encoded_bytes},{"width",saved.frame.width},{"height",saved.frame.height},
    {"host_dq_ns",saved.frame.host_dequeue_mono_ns},{"saved_mono_ns",saved.saved_mono_ns},
    {"source","real_usb_capture"},{"retention","ephemeral bounded cache; invalid after eviction or runtime restart"}};
  return {job.task_id,job.generation,mono(),true,result.dump(),"",saved.frame};
}
ExecutionCompletion AgentTools::model_task(const ExecutionJob& job,const std::function<bool()>& canceled,const std::function<void()>& submitted){
  const bool plan=job.command==TaskCommand::PlanTool;
  if(!model_)throw std::runtime_error("VLM_NOT_CONFIGURED");
  std::optional<SnapshotRecord> snapshot;
  if(!plan)snapshot=snapshots_.query(job.input.snapshot_id,job.generation,mono());
  const auto answer=model_->generate("task-"+std::to_string(job.task_id),plan?plan_system:"Describe only visible evidence. If the image is dark, blurry or insufficient, say so. Do not invent detections, confidences or distances.",
    job.input.prompt,snapshot?snapshot->path:"",plan?1:64,job.deadline_mono_ns,canceled,submitted);
  Json result={{"schema_version",1},{"task_id",job.task_id},{"generation",job.generation},
    {"model","Qwen3-VL-2B-Instruct"},{"model_request_id","task-"+std::to_string(job.task_id)},
    {"started_mono_ns",answer.started_mono_ns},{"completed_mono_ns",answer.finished_mono_ns},
    {"model_response",Json::parse(answer.raw_json)}};
  if(plan){
    auto selected=answer.text;selected.erase(std::remove_if(selected.begin(),selected.end(),[](unsigned char c){return std::isspace(c);}),selected.end());
    if(selected.size()!=1||selected[0]<'0'||selected[0]>'8')throw std::runtime_error("MODEL_TOOL_CHOICE_INVALID");
    const unsigned choice=selected[0]-'0';result["selection_protocol"]="registered-tool-id-v2";result["tool_id"]=choice;result["tool"]=tools[choice];
    result["planning_prompt_revision"]="zh-next-operation-v3";
  }else{
    result["snapshot_id"]=snapshot->id;result["frame_id"]=snapshot->frame.frame_id;
    result["answer"]=answer.text;result["answer_source"]="local_vlm";result["host_dq_ns"]=snapshot->frame.host_dequeue_mono_ns;
  }
  auto text=result.dump();if(text.size()>16384)throw std::runtime_error("AGENT_RESULT_BYTE_BUDGET");
  return {job.task_id,job.generation,mono(),true,std::move(text),"",snapshot?std::optional<FrameMeta>(snapshot->frame):std::nullopt};
}
}
