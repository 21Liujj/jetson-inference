// Real-device task lifecycle acceptance; explicit CUDA injection, no benchmark.
#include "executor_probe_support.h"
using namespace edgevision;
using namespace edgevision::acceptance;
int main(int argc,char** argv){
  std::string out;
  try{
    std::string device,model,labels;bool missing=false; DetectorKind kind=DetectorKind::SsdFruit;
    PreprocessMode preprocess=PreprocessMode::CpuBaseline;
    for(int i=1;i<argc;++i){const std::string key=argv[i];if(key=="--missing-model"){missing=true;continue;}
      if(i+1>=argc)throw std::invalid_argument("option needs value");const std::string value=argv[++i];
      if(key=="--device")device=value;else if(key=="--model")model=value;else if(key=="--labels")labels=value;else if(key=="--out")out=value;
      else if(key=="--kind") { if(value=="yolov8")kind=DetectorKind::YoloV8Coco;else if(value!="ssd")throw std::invalid_argument("unknown model kind"); }
      else if(key=="--preprocess") {if(value=="cuda")preprocess=PreprocessMode::CudaFused;else if(value!="cpu")throw std::invalid_argument("unknown preprocessor");}
      else throw std::invalid_argument("unknown option");}
    require(!device.empty()&&!model.empty()&&!labels.empty()&&!out.empty(),"INVALID_ARGUMENTS");
    require(std::filesystem::create_directories(out),"OUTPUT_ALREADY_EXISTS");
    Harness h(device,out);require(h.router.initialize({model,labels,250,kind,preprocess}).accepted,"MODEL_INITIALIZE_REJECTED");h.start("start-initial");
    if(missing){
      h.wait([&]{return h.router.status().model==ModelState::Error;});auto a=h.detect("missing-model-detect");h.expect(a.task_id,TaskState::Failed);
      require(h.router.query(a.task_id)->error_code=="MODEL_NOT_READY"&&h.router.status().capture==CaptureState::Running,"MODEL_FAILURE_NOT_INDEPENDENT");
      h.log("missing_model_capture_running",a.task_id);++h.cases;
    }else{
      h.wait([&]{return h.router.status().model==ModelState::Ready;},45000000000ULL);
      auto a=h.detect("normal");h.expect(a.task_id,TaskState::Done);h.log("normal_done",a.task_id);
      auto before=h.executor.query().submitted_jobs;auto duplicate=h.detect("normal");h.step();
      require(duplicate.duplicate&&duplicate.task_id==a.task_id&&h.executor.query().submitted_jobs==before,"DUPLICATE_EXECUTED");++h.cases;
      auto queued=h.detect("cancel-before-dispatch");h.router.cancel_pending(now());h.expect(queued.task_id,TaskState::Canceled);
      require(h.executor.query().submitted_jobs==before,"QUEUED_CANCEL_LAUNCHED_GPU");h.log("queued_cancel_without_gpu",queued.task_id);++h.cases;
      h.gate.enabled=true;
      auto expired=h.detect("gpu-deadline",100000000ULL);h.gpu_pending(expired.task_id);
      require(h.router.query(expired.task_id)->state==TaskState::Running,"DEADLINE_PASSED_BEFORE_GPU_OBSERVATION");
      h.log("gpu_pending_before_deadline",expired.task_id);
      h.wait([&]{return h.router.query(expired.task_id)->state==TaskState::Expired;});h.check_pending(expired.task_id,TaskState::Expired,"expired_while_gpu_pending");
      auto waiting=h.detect("queued-behind-expired");h.step();require(h.router.query(waiting.task_id)->state==TaskState::Queued,"EXECUTOR_REUSED_BEFORE_GPU_COMPLETION");
      h.router.cancel_pending(now());h.expect(waiting.task_id,TaskState::Canceled);h.expect(expired.task_id,TaskState::Expired);h.log("expired_physically_drained",expired.task_id);++h.cases;
      auto canceled=h.detect("gpu-cancel");h.gpu_pending(canceled.task_id);require(h.router.cancel(canceled.task_id,now()),"CANCEL_REJECTED");h.step();
      h.check_pending(canceled.task_id,TaskState::Canceled,"canceled_while_gpu_pending");h.expect(canceled.task_id,TaskState::Canceled);h.log("canceled_physically_drained",canceled.task_id);++h.cases;
      auto stopped=h.detect("gpu-stop");h.gpu_pending(stopped.task_id);auto evicted=h.detect("waiting-replaced-by-stop");
      auto at=now();auto stop=h.router.submit({"priority-stop",h.router.status().generation,at+10000000000ULL,TaskCommand::StopCapture,{}},at);
      require(stop.accepted,"STOP_DID_NOT_REPLACE_WAITING_JOB");h.step();h.expect(evicted.task_id,TaskState::Canceled);
      h.wait([&]{auto s=h.capture.snapshot();return s.queue_depth==0&&s.pool_busy==1;},1000000000ULL);
      h.check_pending(stopped.task_id,TaskState::Canceled,"stop_waits_for_inflight_lease");
      require(h.router.status().capture==CaptureState::Stopping&&!h.router.collect(stop.task_id),"STOP_REPORTED_COMPLETE_WITH_GPU_PENDING");
      h.expect(stop.task_id,TaskState::Done);h.expect(stopped.task_id,TaskState::Canceled);
      require(h.capture.snapshot().pool_busy==0&&video_maps()==0,"CAPTURE_RESOURCES_NOT_RELEASED");h.log("stop_physically_done",stop.task_id);++h.cases;
      h.gate.enabled=false;const auto old_generation=h.router.status().generation;h.start("restart-after-gpu-stop");
      const auto restarted=h.router.status();h.log("restart_observed");
      require(restarted.generation>old_generation && restarted.generation-old_generation==1+restarted.start_retries,
              "RESTART_GENERATION_RETRY_ACCOUNTING_MISMATCH");
      auto fresh=h.detect("new-generation-detect");h.expect(fresh.task_id,TaskState::Done);h.log("new_generation_done",fresh.task_id);++h.cases;
      h.gate.enabled=true;auto shutdown_active=h.detect("gpu-shutdown");h.gpu_pending(shutdown_active.task_id);auto shutdown_waiting=h.detect("shutdown-waiting");
      h.router.shutdown(now());h.step();h.check_pending(shutdown_active.task_id,TaskState::Canceled,"shutdown_keeps_gpu_resources");
      auto denied=h.router.submit({"after-shutdown",h.router.status().generation,now()+1000000000ULL,TaskCommand::DetectSnapshot,{}},now());
      require(!denied.accepted&&denied.error==AdmissionError::Busy,"SHUTDOWN_ACCEPTED_NEW_WORK");
      h.router.shutdown(now());h.wait([&]{return h.router.status().shutdown_complete;});
      h.expect(shutdown_active.task_id,TaskState::Canceled);h.expect(shutdown_waiting.task_id,TaskState::Canceled);
      require(h.router.status().model==ModelState::Unloaded&&!h.gate.pending(),"MODEL_OR_GPU_NOT_DRAINED");h.log("shutdown_physically_done",shutdown_active.task_id);++h.cases;
    }
    h.router.shutdown(now());h.wait([&]{return h.router.status().shutdown_complete;});
    require(h.capture.snapshot().pool_busy==0&&video_maps()==0,"FINAL_CAMERA_RESOURCES_LIVE");h.log("final");
    auto e=h.executor.query();std::ofstream summary(out+"/summary.json");
    summary<<"{\"scope\":\"real camera / common task manager / upstream GPU executor lifecycle\",\"cases_passed\":"<<h.cases
      <<",\"explicit_cuda_delay_injection_ms\":200,\"delay_records\":"<<h.gate.records<<",\"gpu_submissions\":"<<e.gpu_submissions
      <<",\"gpu_completions\":"<<e.gpu_completions<<",\"generation\":"<<h.router.status().generation<<",\"pool_busy\":"<<h.capture.snapshot().pool_busy
      <<",\"video_maps\":"<<video_maps()<<",\"max_control_poll_ns_observed\":"<<h.max_poll_ns<<",\"shutdown_complete\":true,\"failure\":\"\"}\n";
    std::cout<<"PASS real executor cases="<<h.cases<<std::endl;return 0;
  }catch(const std::exception& e){std::cerr<<"EXECUTOR_PROBE_FAILED: "<<e.what()<<std::endl;
    if(!out.empty()&&std::filesystem::exists(out)){std::ofstream fail(out+"/failure.json");fail<<"{\"failure\":"<<quote(e.what())<<"}\n";}return 1;}
}
