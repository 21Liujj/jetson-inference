#include "edgevision/upstream_executor.h"
#include "edgevision/nvtx_scope.h"
#include <rclcpp/rclcpp.hpp>
#include <rclcpp_action/rclcpp_action.hpp>
#include <sensor_msgs/msg/image.hpp>
#include <vision_msgs/msg/detection2_d_array.hpp>
#include <diagnostic_msgs/msg/diagnostic_array.hpp>
#include <edgevision_interfaces/msg/frame_meta.hpp>
#include <edgevision_interfaces/srv/set_capture.hpp>
#include <edgevision_interfaces/srv/get_status.hpp>
#include <edgevision_interfaces/action/run_inspection.hpp>
#include <nlohmann/json.hpp>
#include <opencv2/imgproc.hpp>
#include <atomic>
#include <cmath>
#include <fstream>
#include <iostream>
#include <filesystem>
#include <dlfcn.h>
#include <link.h>
#include <thread>
#ifdef EDGEVISION_ROS_GPU_ACCEPTANCE
#include <cuda_runtime_api.h>
extern "C" cudaError_t edgevision_acceptance_delay(unsigned milliseconds);
#endif

namespace ev=edgevision;
using Json=nlohmann::json;
using SetCapture=edgevision_interfaces::srv::SetCapture;
using GetStatus=edgevision_interfaces::srv::GetStatus;
using Inspection=edgevision_interfaces::action::RunInspection;
using Goal=rclcpp_action::ServerGoalHandle<Inspection>;
uint64_t mono(){return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();}
const char* text(ev::CaptureState s){const char* n[]={"IDLE","STARTING","RUNNING","STOPPING","RECOVERING","ERROR"};return n[unsigned(s)];}
const char* text(ev::ModelState s){const char* n[]={"UNLOADED","LOADING","READY","BUSY","ERROR"};return n[unsigned(s)];}
const char* text(ev::TaskState s){const char* n[]={"QUEUED","RUNNING","DONE","FAILED","EXPIRED","CANCELED"};return n[unsigned(s)];}
bool external_id(const std::string& id){
  if(id.empty()||id.size()>64||id.rfind("ros-auto-",0)==0||id.rfind("ros-boot-",0)==0)return false;
  for(unsigned char c:id)if(!((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='-'||c=='_'||c=='.'||c==':'))return false;
  return true;
}
std::optional<ev::TaskCommand> inspection_command(const std::string& operation){
  if(operation=="detect")return ev::TaskCommand::DetectSnapshot;
  if(operation=="snapshot")return ev::TaskCommand::CaptureSnapshot;
  if(operation=="describe")return ev::TaskCommand::DescribeSnapshot;
  if(operation=="plan")return ev::TaskCommand::PlanTool;
  if(operation=="resume_detector")return ev::TaskCommand::ResumeDetector;
  return std::nullopt;
}

class EdgeVisionNode final:public rclcpp::Node {
 public:
  EdgeVisionNode():Node("edgevision") {
    std::string boot_id;std::ifstream boot("/proc/sys/kernel/random/boot_id");std::getline(boot,boot_id);
    runtime_instance_=boot_id.substr(0,64)+"-"+std::to_string(mono());
    // ament installs executables with a different loader search path. Refuse an
    // old system jetson-inference before starting the model worker.
    for(const char* name:{"libjetson-inference.so","libjetson-utils.so"}){
      void* library=dlopen(name,RTLD_NOW|RTLD_NOLOAD);link_map* mapping=nullptr;
      if(!library || dlinfo(library,RTLD_DI_LINKMAP,&mapping)!=0 || !mapping){
        if(library)dlclose(library);throw std::runtime_error("UPSTREAM_LIBRARY_NOT_LOADED");}
      const auto loaded=std::filesystem::canonical(mapping->l_name);
      const auto expected=std::filesystem::canonical(std::filesystem::path(EDGEVISION_VERIFIED_UPSTREAM_PREFIX)/"lib"/name);
      dlclose(library);
      if(loaded!=expected)throw std::runtime_error("UPSTREAM_RUNTIME_PATH_MISMATCH:"+loaded.string());
      RCLCPP_INFO(get_logger(),"verified upstream runtime: %s",loaded.c_str());
    }
    if(get_parameter("use_sim_time").as_bool())throw std::invalid_argument("LIVE_CAPTURE_REQUIRES_SYSTEM_ROS_CLOCK");
    config_.device=declare_parameter<std::string>("device","");
    model_=declare_parameter<std::string>("model","");labels_=declare_parameter<std::string>("labels","");
    image_hz_=declare_parameter<double>("image_hz",5.0);detect_hz_=declare_parameter<double>("detection_hz",5.0);
    auto_start_=declare_parameter<bool>("auto_start",true);
    agent_config_.snapshot_root=declare_parameter<std::string>("snapshot_root","");
    agent_config_.vlm_binary=declare_parameter<std::string>("vlm_binary","");
    agent_config_.llm_engine_dir=declare_parameter<std::string>("vlm_llm_dir","");
    agent_config_.visual_engine_dir=declare_parameter<std::string>("vlm_visual_dir","");
    agent_config_.plugin_path=declare_parameter<std::string>("vlm_plugin","");
    const bool diagnostics=declare_parameter<bool>("publish_diagnostics",true);
    const bool paired=declare_parameter<bool>("publish_detection_image",true);
    publish_detections_=declare_parameter<bool>("publish_detections",true);
    frame_name_=declare_parameter<std::string>("optical_frame","camera_optical_frame");
    if(!ev::ControlRouter::valid_config(config_) || model_.empty() || labels_.empty() ||
       !std::isfinite(image_hz_) || image_hz_<0 || image_hz_>30 ||
       !std::isfinite(detect_hz_) || detect_hz_<0 || detect_hz_>30 || frame_name_.empty())
      throw std::invalid_argument("INVALID_RUNTIME_PARAMETERS");
    if(image_hz_>0)preview_=std::make_shared<ev::PreviewCache>(4*1024*1024,uint64_t(1e9/image_hz_));
    if(paired)inference_preview_=std::make_shared<ev::PreviewCache>(4*1024*1024,1);
    capture_=std::make_unique<ev::V4L2Backend>(std::shared_ptr<ev::V4L2Io>{},preview_);
    std::function<void()> acceptance_hook;
#ifdef EDGEVISION_ROS_GPU_ACCEPTANCE
    acceptance_hook=[] {const auto e=edgevision_acceptance_delay(200);
      if(e!=cudaSuccess)throw std::runtime_error(cudaGetErrorString(e));};
    RCLCPP_WARN(get_logger(),"EXPLICIT_GPU_ACCEPTANCE_INJECTION_200MS; not a performance run");
#endif
    executor_=std::make_unique<ev::UpstreamExecutor>(*capture_,&health_,acceptance_hook,inference_preview_);
    router_=std::make_unique<ev::ControlRouter>(*capture_,executor_.get(),4096);
    data_group_=create_callback_group(rclcpp::CallbackGroupType::MutuallyExclusive);
    control_group_=create_callback_group(rclcpp::CallbackGroupType::Reentrant);
    diag_group_=create_callback_group(rclcpp::CallbackGroupType::MutuallyExclusive);
    action_group_=create_callback_group(rclcpp::CallbackGroupType::Reentrant);
    auto qos=rclcpp::SensorDataQoS().keep_last(2);
    images_=create_publisher<sensor_msgs::msg::Image>("/edgevision/image",qos);
    paired_images_=create_publisher<sensor_msgs::msg::Image>("/edgevision/detection_image",qos);
    detections_=create_publisher<vision_msgs::msg::Detection2DArray>("/edgevision/detections",qos);
    metadata_=create_publisher<edgevision_interfaces::msg::FrameMeta>("/edgevision/frame_meta",qos);
    diagnostics_=create_publisher<diagnostic_msgs::msg::DiagnosticArray>("/diagnostics",rclcpp::QoS(5).reliable());
    set_capture_=create_service<SetCapture>("/edgevision/set_capture",[this](SetCapture::Request::SharedPtr req,SetCapture::Response::SharedPtr res){
      if(!external_id(req->request_id) || req->timeout_ms<100 || req->timeout_ms>30000 || (req->enable && req->config_id!="usb-yuyv-640x360-30")){
        res->error_code="INVALID_PARAMETERS";return;}
      const auto at=mono();auto s=router_->status();
      auto a=router_->submit({req->request_id,req->generation,at+uint64_t(req->timeout_ms)*1000000,
        req->enable?ev::TaskCommand::StartCapture:ev::TaskCommand::StopCapture,
        req->enable?std::optional<ev::CaptureConfig>(config_):std::nullopt},at);
      res->accepted=a.accepted;res->duplicate=a.duplicate;res->task_id=a.task_id;
      if(!a.accepted)res->error_code="ADMISSION_"+std::to_string(unsigned(a.error));
    },rmw_qos_profile_services_default,control_group_);
    get_status_=create_service<GetStatus>("/edgevision/get_status",[this](GetStatus::Request::SharedPtr req,GetStatus::Response::SharedPtr res){
      auto s=router_->status();auto c=capture_->snapshot();res->schema_version=1;
      res->capture_state=text(s.capture);res->model_state=text(s.model);res->generation=s.generation;
      res->resources_live=s.backend_resources_live;res->queue_depth=c.queue_depth;res->pool_busy=c.pool_busy;
      res->capture_error=s.last_error;res->model_error=s.model_error;res->metrics_json=metrics().dump();
      if(auto t=router_->query(req->task_id)){res->task_found=true;res->task_state=text(t->state);
        res->execution_pending=t->execution_pending;res->task_error=t->error_code;res->result_json=t->result;}
    },rmw_qos_profile_services_default,control_group_);
    action_=rclcpp_action::create_server<Inspection>(this,"/edgevision/run_inspection",
      [this](const rclcpp_action::GoalUUID&,std::shared_ptr<const Inspection::Goal> goal){
        bool idle=false;
        if(!inspection_command(goal->operation) || !external_id(goal->request_id) || goal->timeout_ms<1 || goal->timeout_ms>30000 ||
           !goal->freshness_ms || goal->freshness_ms>5000 ||
           !goal_reserved_.compare_exchange_strong(idle,true))return rclcpp_action::GoalResponse::REJECT;
        return rclcpp_action::GoalResponse::ACCEPT_AND_EXECUTE;},
      [](std::shared_ptr<Goal>){return rclcpp_action::CancelResponse::ACCEPT;},
      [this](std::shared_ptr<Goal> goal){std::lock_guard<std::mutex> lock(goal_mu_);goal_=std::move(goal);goal_task_=0;},
      rcl_action_server_get_default_options(),action_group_);
    if(preview_)image_timer_=create_wall_timer(std::chrono::nanoseconds(uint64_t(1e9/image_hz_)),[this]{publish_preview();},data_group_);
    if(diagnostics)diag_timer_=create_wall_timer(std::chrono::seconds(1),[this]{publish_diagnostics();},diag_group_);
    owner_=std::thread(&EdgeVisionNode::control_loop,this);
  }
  ~EdgeVisionNode() override {closing_=true;if(owner_.joinable())owner_.join();}
 private:
  Json metrics(){auto s=router_->status();auto h=health_.snapshot(mono(),s);
    Json j={{"valid_frames",h.counters.valid_frames},{"pool_drop",h.counters.pool_drops},{"ready_drop",h.counters.ready_drops},
            {"sequence_gaps",h.counters.sequence_gaps},{"queue_depth",h.counters.queue_depth},{"pool_busy",h.counters.pool_busy},
            {"preview_copied",preview_?preview_->copied():0},{"preview_busy_skips",preview_?preview_->skipped_busy():0},
            {"detection_results_published",results_published_.load()},{"paired_image_skips",pair_skips_.load()},
            {"latency_clock","CLOCK_MONOTONIC"},{"stamp_source","publication_ros_time"},{"camera_calibrated",false}};
    j["runtime_instance"]=runtime_instance_;
    const auto execution=executor_->query();j["gpu_submissions"]=execution.gpu_submissions;
    j["gpu_completions"]=execution.gpu_completions;j["physical_busy"]=execution.physical_busy;
    j["gpu_task_id"]=execution.task_id?Json(*execution.task_id):Json(nullptr);
    j["gpu_submitted_mono_ns"]=execution.gpu_submitted_mono_ns;
    j["vlm_model"]=text(execution.vlm_model);j["vlm_requests"]=execution.vlm_requests;j["vlm_error"]=execution.vlm_error;
    j["active_backend"]=execution.active_backend;j["model_switches"]=execution.model_switches;
    j["cuda_context_releases"]=execution.cuda_context_releases;
    j["detector_unloaded_mono_ns"]=execution.detector_unloaded_mono_ns;j["detector_ready_mono_ns"]=execution.detector_ready_mono_ns;
    j["capture_fps"]=h.rate.valid_capture_fps?Json(*h.rate.valid_capture_fps):Json(nullptr);
    j["dequeue_to_result_p95_ns"]=h.dequeue_to_result.p95_ns?Json(*h.dequeue_to_result.p95_ns):Json(nullptr);
    j["dequeue_to_result_p99_ns"]=h.dequeue_to_result.p99_ns?Json(*h.dequeue_to_result.p99_ns):Json(nullptr);return j;}
  void publish_image(const ev::Frame& f,const std_msgs::msg::Header& header,
      const rclcpp::Publisher<sensor_msgs::msg::Image>::SharedPtr& publisher){
    if(f.pixfmt_fourcc!=ev::make_fourcc('Y','U','Y','V') || !f.width || f.width%2 || !f.height ||
       f.stride<f.width*2 || f.bytesused<uint64_t(f.height-1)*f.stride+f.width*2)throw std::runtime_error("INVALID_PREVIEW_LAYOUT");
    cv::Mat packed(f.height,f.width,CV_8UC2,const_cast<uint8_t*>(f.data.data()),f.stride),rgb;
    cv::cvtColor(packed,rgb,cv::COLOR_YUV2RGB_YUY2);
    sensor_msgs::msg::Image msg;msg.header=header;msg.height=f.height;msg.width=f.width;msg.encoding="rgb8";
    msg.step=f.width*3;msg.is_bigendian=false;msg.data.assign(rgb.datastart,rgb.dataend);
    ev::NvtxScope range("publish/ros-image-owned-copy");publisher->publish(std::move(msg));
  }
  void publish_meta(const ev::FrameMeta& f,const std_msgs::msg::Header& header,const char* source,uint64_t task=0,uint64_t completed=0){
    edgevision_interfaces::msg::FrameMeta msg;msg.header=header;msg.schema_version=1;msg.source=source;
    msg.stamp_source="publication_ros_time";msg.capture_frame_id=f.frame_id;msg.generation=f.session_generation;msg.task_id=task;
    msg.driver_sequence=f.driver_sequence;msg.driver_flags=f.driver_flags;
    msg.driver_timestamp_valid=f.driver_timestamp_ns.has_value();msg.driver_timestamp_ns=f.driver_timestamp_ns.value_or(0);
    msg.driver_clock=f.driver_timestamp_clock==ev::FrameMeta::TimestampClock::Monotonic?"CLOCK_MONOTONIC":"unknown";
    msg.driver_timestamp_source=f.driver_timestamp_source==ev::FrameMeta::TimestampSource::EndOfFrame?"end_of_frame":
      f.driver_timestamp_source==ev::FrameMeta::TimestampSource::StartOfExposure?"start_of_exposure":"unknown";
    msg.host_dequeue_mono_ns=f.host_dequeue_mono_ns;msg.core_publish_mono_ns=f.publish_mono_ns;msg.result_completed_mono_ns=completed;
    msg.width=f.width;msg.height=f.height;msg.fourcc=f.pixfmt_fourcc;msg.stride=f.stride;msg.bytesused=f.bytesused;metadata_->publish(msg);
  }
  void publish_preview(){
    ev::Frame f;const auto s=router_->status();
    if(s.capture!=ev::CaptureState::Running || !preview_->read(f) || f.session_generation!=s.generation ||
       (f.frame_id==last_preview_ && f.session_generation==last_preview_generation_) ||
       mono()<f.host_dequeue_mono_ns || mono()-f.host_dequeue_mono_ns>500000000)return;
    std_msgs::msg::Header header;header.stamp=now();header.frame_id=frame_name_;
    try{publish_image(f,header,images_);publish_meta(f,header,"preview");last_preview_=f.frame_id;last_preview_generation_=f.session_generation;}
    catch(const std::exception& e){RCLCPP_ERROR(get_logger(),"preview: %s",e.what());}
  }
  void publish_result(const ev::TaskRecord& record){
    if(!publish_detections_ || record.state!=ev::TaskState::Done || record.result.empty())return;
    auto data=Json::parse(record.result);if(data.at("generation").get<uint64_t>()!=router_->status().generation)return;
    vision_msgs::msg::Detection2DArray msg;msg.header.stamp=now();msg.header.frame_id=frame_name_;
    for(const auto& b:data.at("detections")){
      vision_msgs::msg::Detection2D box;box.header=msg.header;
      vision_msgs::msg::ObjectHypothesisWithPose hypothesis;hypothesis.hypothesis.class_id=std::to_string(b.at("class_id").get<int>());
      hypothesis.hypothesis.score=b.at("confidence").get<double>();box.results.push_back(hypothesis);
      const double l=b.at("left"),r=b.at("right"),t=b.at("top"),d=b.at("bottom");
      box.bbox.center.position.x=(l+r)/2;box.bbox.center.position.y=(t+d)/2;box.bbox.center.theta=0;
      box.bbox.size_x=r-l;box.bbox.size_y=d-t;msg.detections.push_back(std::move(box));}
    ev::Frame f;bool paired=inference_preview_ && inference_preview_->read(f) &&
      f.frame_id==data.at("frame_id").get<uint64_t>() && f.session_generation==data.at("generation").get<uint64_t>();
    const char* source=record.request.input.snapshot_id.empty()?"detections":"snapshot_detection";
    if(paired){publish_image(f,msg.header,paired_images_);publish_meta(f,msg.header,source,record.id,data.at("completed_mono_ns"));}
    else{
      ++pair_skips_;ev::FrameMeta meta;meta.frame_id=data.at("frame_id");meta.session_generation=data.at("generation");
      meta.host_dequeue_mono_ns=data.at("host_dq_ns");meta.width=data.at("width");meta.height=data.at("height");
      meta.driver_sequence=data.at("sequence");meta.driver_flags=data.at("driver_flags");
      if(data.at("driver_timestamp_valid").get<bool>())meta.driver_timestamp_ns=data.at("driver_timestamp_ns").get<uint64_t>();
      meta.driver_timestamp_clock=data.at("driver_clock_monotonic").get<bool>()?ev::FrameMeta::TimestampClock::Monotonic:ev::FrameMeta::TimestampClock::Unknown;
      meta.driver_timestamp_source=static_cast<ev::FrameMeta::TimestampSource>(data.at("driver_timestamp_source").get<int>());
      meta.stride=data.at("stride");meta.bytesused=data.at("bytesused");meta.pixfmt_fourcc=data.at("fourcc");meta.publish_mono_ns=data.at("publish_mono_ns");
      publish_meta(meta,msg.header,source,record.id,data.at("completed_mono_ns"));}
    ev::NvtxScope range("publish/ros-detections");detections_->publish(msg);++results_published_;
  }
  void publish_diagnostics(){
    const auto s=router_->status();diagnostic_msgs::msg::DiagnosticArray array;array.header.stamp=now();
    for(const auto& kind:{std::string("capture"),std::string("model")}){
      diagnostic_msgs::msg::DiagnosticStatus status;status.name="edgevision/"+kind;status.hardware_id=config_.device;
      const bool bad=kind=="capture"?s.capture==ev::CaptureState::Error:s.model==ev::ModelState::Error;
      status.level=bad?diagnostic_msgs::msg::DiagnosticStatus::ERROR:diagnostic_msgs::msg::DiagnosticStatus::OK;
      status.message=kind=="capture"?text(s.capture):text(s.model);
      diagnostic_msgs::msg::KeyValue value;value.key="metrics_json";value.value=metrics().dump();status.values.push_back(value);
      value.key="last_error";value.value=kind=="capture"?s.last_error:s.model_error;status.values.push_back(value);array.status.push_back(status);}
    diagnostics_->publish(array);
  }
  void action_step(uint64_t at){
    std::lock_guard<std::mutex> lock(goal_mu_);if(!goal_)return;
    if(!goal_task_){const auto g=goal_->get_goal();auto s=router_->status();
      auto a=router_->submit({g->request_id,g->generation,at+uint64_t(g->timeout_ms)*1000000,*inspection_command(g->operation),{},
        {g->snapshot_id,g->prompt,g->freshness_ms}},at);
      if(!a.accepted){auto r=std::make_shared<Inspection::Result>();r->state="FAILED";r->error_code="ADMISSION_"+std::to_string(unsigned(a.error));
        goal_->abort(r);goal_.reset();goal_reserved_=false;return;}goal_task_=a.task_id;}
    if(goal_->is_canceling())router_->cancel(goal_task_,at);
    if(auto done=router_->collect(goal_task_)){
      auto result=std::make_shared<Inspection::Result>();result->task_id=done->id;result->state=text(done->state);
      result->success=done->state==ev::TaskState::Done;result->error_code=done->error_code;result->result_json=done->result;
      if(result->success && goal_->get_goal()->operation=="detect")publish_result(*done);
      if(goal_->is_canceling())goal_->canceled(result);else if(result->success)goal_->succeed(result);else goal_->abort(result);
      goal_.reset();goal_task_=0;goal_reserved_=false;
    }else if(at>=next_feedback_){auto feedback=std::make_shared<Inspection::Feedback>();feedback->task_id=goal_task_;
      feedback->generation=router_->status().generation;if(auto t=router_->query(goal_task_)){feedback->state=text(t->state);feedback->execution_pending=t->execution_pending;}
      goal_->publish_feedback(feedback);next_feedback_=at+100000000;}
  }
  void control_loop(){
    try{
      auto init=router_->initialize({model_,labels_,250,ev::DetectorKind::YoloV8Coco,ev::PreprocessMode::CudaFused,agent_config_});
      if(!init.accepted)throw std::runtime_error(init.error);
      const auto started=mono();
      if(auto_start_)router_->submit({"ros-boot-"+std::to_string(started),0,started+30000000000ULL,ev::TaskCommand::StartCapture,config_},started);
      while(!closing_ && rclcpp::ok()){
        const auto at=mono();router_->poll(at);
        if(at>=next_health_){health_.observe_capture(capture_->health_sample(at));next_health_=at+100000000;}
        if(auto_task_){if(auto done=router_->collect(*auto_task_)){publish_result(*done);router_->release_internal_result(*auto_task_);auto_task_.reset();}}
        action_step(at);const auto state=router_->status();
        if(detect_hz_>0 && !auto_task_ && !goal_reserved_ && at>=next_detect_ && state.capture==ev::CaptureState::Running && state.model==ev::ModelState::Ready){
          auto a=router_->submit({"ros-auto-"+std::to_string(++auto_sequence_),state.generation,at+2000000000ULL,ev::TaskCommand::DetectSnapshot,{}},at);
          if(a.accepted)auto_task_=a.task_id;
          const auto period=uint64_t(1e9/detect_hz_);next_detect_=next_detect_?next_detect_+period:at+period;
          if(next_detect_<=at)next_detect_=at+period;}
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
    }catch(const std::exception& e){RCLCPP_ERROR(get_logger(),"runtime owner: %s",e.what());}
    const auto stop=mono();router_->shutdown(stop);
    while(!router_->status().shutdown_complete){router_->poll(mono());
      if(mono()-stop>20000000000ULL){std::fputs("ROS_RUNTIME_PHYSICAL_SHUTDOWN_UNKNOWN\n",stderr);std::_Exit(71);}
      std::this_thread::sleep_for(std::chrono::milliseconds(1));}
    const auto final=capture_->snapshot();
    std::cout<<"EDGEVISION_RUNTIME_STOPPED "<<Json({{"shutdown_complete",true},{"pool_busy",final.pool_busy},
      {"worker_active",final.worker_active},{"resources_live",router_->status().backend_resources_live}}).dump()<<std::endl;
  }
  ev::CaptureConfig config_;std::string model_,labels_,frame_name_;double image_hz_=5,detect_hz_=5;bool auto_start_=true,publish_detections_=true;
  ev::AgentBackendConfig agent_config_;
  std::string runtime_instance_;
  std::shared_ptr<ev::PreviewCache> preview_,inference_preview_;std::unique_ptr<ev::V4L2Backend> capture_;
  ev::HealthMonitor health_;std::unique_ptr<ev::UpstreamExecutor> executor_;std::unique_ptr<ev::ControlRouter> router_;
  std::atomic<bool> closing_{false},goal_reserved_{false};std::thread owner_;
  std::mutex goal_mu_;std::shared_ptr<Goal> goal_;uint64_t goal_task_=0,next_feedback_=0;
  std::optional<uint64_t> auto_task_;uint64_t auto_sequence_=0,next_detect_=0,next_health_=0,last_preview_=0,last_preview_generation_=0;
  std::atomic<uint64_t> results_published_{0},pair_skips_{0};
  rclcpp::CallbackGroup::SharedPtr data_group_,control_group_,diag_group_,action_group_;
  rclcpp::Publisher<sensor_msgs::msg::Image>::SharedPtr images_,paired_images_;
  rclcpp::Publisher<vision_msgs::msg::Detection2DArray>::SharedPtr detections_;
  rclcpp::Publisher<edgevision_interfaces::msg::FrameMeta>::SharedPtr metadata_;
  rclcpp::Publisher<diagnostic_msgs::msg::DiagnosticArray>::SharedPtr diagnostics_;
  rclcpp::Service<SetCapture>::SharedPtr set_capture_;rclcpp::Service<GetStatus>::SharedPtr get_status_;
  rclcpp_action::Server<Inspection>::SharedPtr action_;rclcpp::TimerBase::SharedPtr image_timer_,diag_timer_;
};

int main(int argc,char** argv){
  rclcpp::init(argc,argv);
  try{auto node=std::make_shared<EdgeVisionNode>();rclcpp::executors::MultiThreadedExecutor executor(rclcpp::ExecutorOptions(),4);
    executor.add_node(node);executor.spin();executor.remove_node(node);node.reset();rclcpp::shutdown();return 0;}
  catch(const std::exception& e){std::fprintf(stderr,"EDGEVISION_ROS_FAILED: %s\n",e.what());rclcpp::shutdown();return 1;}
}
