#include "control_fixture.h"
#include <cstdio>
#include <functional>

struct ExecutorFixture final : ExecutionBackend {
  ExecutionSnapshot state;
  std::optional<ExecutionJob> active;
  std::optional<ExecutionCompletion> completion;
  std::function<void()> inspect;
  unsigned submissions=0, cancels=0, shutdowns=0;
  bool reject=false, throw_submit=false, throw_collect=false;
  bool agent=false;
  bool supports_command(TaskCommand c)const noexcept override {
    return c==TaskCommand::DetectSnapshot || (agent && (c==TaskCommand::CaptureSnapshot || c==TaskCommand::PlanTool || c==TaskCommand::DescribeSnapshot || c==TaskCommand::ResumeDetector));
  }
  bool automatic_model_switching()const noexcept override{return agent;}
  ExecutionReply initialize(InferenceConfig) override { state.model=ModelState::Ready; return {true,""}; }
  ExecutionReply submit(ExecutionJob job) override {
    if (inspect) inspect(); ++submissions;
    if (reject) return {false,"FIXTURE_REJECTION"};
    active=job; state.physical_busy=true;
    if (throw_submit) throw std::runtime_error("explicit partial-submit fixture");
    return {true,""};
  }
  ExecutionSnapshot query() const override { if (inspect) inspect(); return state; }
  std::optional<ExecutionCompletion> collect() override {
    if (inspect) inspect();
    if (throw_collect) { throw_collect=false; throw std::runtime_error("explicit collect fixture"); }
    auto out=std::move(completion);completion.reset();return out;
  }
  void cancel_pending(uint64_t id) noexcept override { if (inspect) inspect();if(active)assert(active->task_id==id);++cancels; }
  void shutdown() noexcept override { if (inspect) inspect();++shutdowns; }
  void finish(bool success=true) {
    assert(active); ExecutionCompletion c;c.task_id=active->task_id;c.generation=active->generation;
    c.success=success;c.result=success?"{\"detections\":[]}":"";c.error=success?"":"FIXTURE_FAILURE";
    completion=std::move(c);active.reset();state.physical_busy=false;
  }
};
static ControlRequest detect(std::string id,uint64_t deadline=10000,uint64_t generation=1) {
  return {std::move(id),generation,deadline,TaskCommand::DetectSnapshot,{}};
}
static void running(ControlRouter& router,Fixture& camera) {
  assert(router.initialize({"explicit-fixture.onnx","explicit-fixture.labels",250}).accepted);
  auto a=router.submit(start("start"),1);assert(a.accepted);router.poll(2);camera.first(1);router.poll(3);
  assert(router.query(a.task_id)->state==TaskState::Done && router.status().capture==CaptureState::Running);
}
int main() {
  {
    Fixture camera;ExecutorFixture executor;executor.agent=true;ControlRouter router(camera,&executor);
    assert(router.initialize({"fixture","labels",250}).accepted);
    ControlRequest plan{"plan",0,10000,TaskCommand::PlanTool,{}, {"","read status",500}};
    auto a=router.submit(plan,1);assert(a.accepted);router.poll(2);
    assert(executor.active && executor.active->command==TaskCommand::PlanTool && executor.active->input.prompt=="read status");
    assert(router.status().capture==CaptureState::Idle && camera.starts==0);
    auto changed=plan;changed.input.prompt="stop capture";assert(router.submit(changed,3).error==AdmissionError::Conflict);
    assert(router.cancel(a.task_id,4));router.poll(5);assert(!router.collect(a.task_id));
    executor.finish();router.poll(6);assert(router.collect(a.task_id)->state==TaskState::Canceled);
    auto snapshot=router.submit({"snapshot-no-camera",0,10000,TaskCommand::CaptureSnapshot,{}},7);
    assert(snapshot.accepted);router.poll(8);assert(router.collect(snapshot.task_id)->error_code=="CAPTURE_NOT_RUNNING");
    ControlRequest stored{"stored",0,10000,TaskCommand::DetectSnapshot,{}, {"g0-f0-t1","",500}};
    auto image=router.submit(stored,9);assert(image.accepted);router.poll(10);
    assert(executor.active && executor.active->input.snapshot_id=="g0-f0-t1");
    executor.finish();router.poll(11);assert(router.collect(image.task_id)->state==TaskState::Done);
  }
  {
    Fixture camera;ExecutorFixture executor;ControlRouter router(camera,&executor);running(router,camera);
    executor.inspect=[&]{(void)router.status();}; // no backend call under router lock
    auto a=router.submit(detect("normal"),4);assert(a.accepted);router.poll(5);
    assert(router.query(a.task_id)->state==TaskState::Running && !router.collect(a.task_id));
    auto duplicate=router.submit(detect("normal",99999),6);assert(duplicate.duplicate && duplicate.task_id==a.task_id);
    executor.finish();router.poll(7);auto result=router.collect(a.task_id);
    assert(result && result->state==TaskState::Done && !result->execution_pending && !result->result.empty());
    assert(executor.submissions==1 && router.status().capture==CaptureState::Running);
  }
  {
    Fixture camera;ExecutorFixture executor;ControlRouter router(camera,&executor);running(router,camera);
    auto a=router.submit(detect("cancel-active"),4);router.poll(5);
    auto b=router.submit(detect("waiting"),6);assert(b.accepted);
    assert(router.submit(detect("full"),6).error==AdmissionError::Busy);
    assert(router.cancel(a.task_id,7));router.poll(8);
    assert(router.query(a.task_id)->state==TaskState::Canceled && router.query(a.task_id)->execution_pending);
    assert(!router.collect(a.task_id) && executor.submissions==1 && executor.cancels==1);
    router.cancel_pending(9);assert(router.collect(b.task_id)->state==TaskState::Canceled);
    auto again=router.submit(detect("cancel-active"),10);assert(again.duplicate && executor.submissions==1);
    executor.finish();router.poll(11);auto result=router.collect(a.task_id);
    assert(result && result->state==TaskState::Canceled && result->result.empty());
  }
  {
    Fixture camera;ExecutorFixture executor;ControlRouter router(camera,&executor);running(router,camera);
    auto a=router.submit(detect("expires",10),4);router.poll(5);router.poll(10);
    assert(router.query(a.task_id)->state==TaskState::Expired && router.query(a.task_id)->execution_pending && !router.collect(a.task_id));
    executor.finish();router.poll(11);auto result=router.collect(a.task_id);
    assert(result && result->state==TaskState::Expired && result->result.empty());
  }
  for(bool cleanup_fault:{false,true}) {
    Fixture camera;ExecutorFixture executor;ControlRouter router(camera,&executor);running(router,camera);
    auto a=router.submit(detect("in-flight"),4);router.poll(5);auto b=router.submit(detect("waiting"),6);assert(b.accepted);
    auto bad=stop("bad-stop",2);assert(router.submit(bad,7).error==AdmissionError::StaleGeneration);
    bad=stop("bad-stop",1);bad.deadline_mono_ns=7;assert(router.submit(bad,7).error==AdmissionError::DeadlineExceeded);
    assert(router.query(b.task_id)->state==TaskState::Queued);
    auto s=router.submit(stop("priority-stop",1),8);assert(s.accepted && router.query(b.task_id)->state==TaskState::Canceled);
    assert(router.submit(detect("after-stop"),9).error==AdmissionError::Busy);
    assert(router.submit(stop("priority-stop",1),9).duplicate);
    router.poll(10);assert(camera.stops==1 && router.status().capture==CaptureState::Stopping);
    assert(router.query(a.task_id)->state==TaskState::Canceled && router.query(a.task_id)->execution_pending);
    if(cleanup_fault) {CaptureEvent e;e.kind=CaptureEvent::Kind::Fault;e.generation=1;e.error_code="CLEANUP_FAILURE";camera.events.push_back(e);}
    camera.stopped(1);router.poll(11);assert(!router.collect(s.task_id));
    executor.finish();router.poll(12);auto stopped=router.collect(s.task_id);
    assert(stopped && stopped->state==(cleanup_fault?TaskState::Failed:TaskState::Done));
    assert(!cleanup_fault || stopped->error_code=="CLEANUP_FAILURE");
    assert(router.collect(a.task_id)->result.empty());
  }
  {
    Fixture camera;ExecutorFixture executor;ControlRouter router(camera,&executor);running(router,camera);
    auto a=router.submit(detect("old-generation",10000000000ULL),4);router.poll(5);
    CaptureEvent fault;fault.kind=CaptureEvent::Kind::Fault;fault.generation=1;fault.runtime_retryable=true;fault.error_code="ENODEV";
    camera.events.push_back(fault);camera.stopped(1);router.poll(6);router.poll(1000000006ULL);
    assert(router.status().generation==2 && router.query(a.task_id)->execution_pending);
    executor.finish();router.poll(1000000007ULL);auto result=router.collect(a.task_id);
    assert(result && result->state==TaskState::Failed && result->error_code=="STALE_GENERATION" && result->result.empty());
    camera.first(2);router.poll(1000000008ULL);assert(router.status().capture==CaptureState::Running);
  }
  for(int failure=0;failure<3;++failure) {
    Fixture camera;ExecutorFixture executor;ControlRouter router(camera,&executor);running(router,camera);
    executor.reject=failure==0;executor.throw_submit=failure==1;
    auto a=router.submit(detect("failure"),4);router.poll(5);
    if(failure==2) {executor.throw_collect=true;router.poll(6);}
    assert(router.query(a.task_id)->state==TaskState::Failed);
    if(failure) {assert(router.query(a.task_id)->execution_pending && !router.collect(a.task_id));executor.finish();router.poll(7);}
    assert(router.collect(a.task_id)->state==TaskState::Failed && router.collect(a.task_id)->result.empty());
    assert(router.status().capture==CaptureState::Running);
  }
  {
    Fixture camera;ExecutorFixture executor;ControlRouter router(camera,&executor);running(router,camera);
    auto a=router.submit(detect("shutdown-active"),4);router.poll(5);auto b=router.submit(detect("shutdown-waiting"),6);
    router.shutdown(7);assert(router.status().shutting_down && !router.status().shutdown_complete && camera.stops==1);
    assert(router.query(a.task_id)->execution_pending && router.collect(b.task_id)->state==TaskState::Canceled);
    assert(router.submit(detect("closed"),8).error==AdmissionError::Busy);
    assert(router.submit(detect("shutdown-active"),8).duplicate);
    camera.stopped(1);executor.finish();executor.state.stopped=true;executor.state.model=ModelState::Unloaded;
    router.poll(9);assert(router.status().shutdown_complete && router.collect(a.task_id)->state==TaskState::Canceled);
    router.shutdown(10);router.poll(11);assert(camera.stops==1 && router.status().shutdown_complete);
  }
  {
    Fixture camera;ExecutorFixture executor;ControlRouter router(camera,&executor);running(router,camera);
    auto a=router.submit(detect("capture-failed"),4);router.poll(5);
    CaptureEvent fault;fault.kind=CaptureEvent::Kind::Fault;fault.generation=1;fault.error_code="CAPTURE_FATAL";
    camera.events.push_back(fault);camera.stopped(1);executor.finish();router.poll(6);
    auto result=router.collect(a.task_id);
    assert(result && result->state==TaskState::Failed && result->error_code=="CAPTURE_NOT_RUNNING" && result->result.empty());
    assert(router.status().capture==CaptureState::Error && router.status().model==ModelState::Ready);
  }
  {
    Fixture camera;ExecutorFixture executor;ControlRouter router(camera,&executor);
    router.initialize({"fixture","labels",250});auto a=router.submit(start("shutdown-backoff",0,100000000000ULL),1);router.poll(2);
    CaptureEvent fault;fault.kind=CaptureEvent::Kind::Fault;fault.generation=1;fault.retryable=true;fault.error_code="EIO";
    camera.events.push_back(fault);camera.stopped(1);router.poll(3);assert(router.status().capture==CaptureState::Recovering);
    router.shutdown(4);executor.state.stopped=true;executor.state.model=ModelState::Unloaded;router.poll(5);
    assert(router.status().capture==CaptureState::Idle && router.status().shutdown_complete);
    assert(router.collect(a.task_id)->state==TaskState::Canceled);
  }
  std::puts("PASS one task manager: detection/dedup, logical-vs-physical completion, stop priority, cleanup faults, generation, executor failures and shutdown; explicit CPU fixtures only");
}
