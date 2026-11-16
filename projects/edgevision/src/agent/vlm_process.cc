#include "edgevision/vlm_process.h"
#include "edgevision/nvtx_scope.h"
#include <nlohmann/json.hpp>
#include <chrono>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <fcntl.h>
#include <poll.h>
#include <spawn.h>
#include <stdexcept>
#include <sys/socket.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>
extern char** environ;

namespace edgevision {
namespace {
using Json=nlohmann::json;
namespace fs=std::filesystem;
uint64_t mono(){return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();}
void close_fd(int& fd)noexcept{if(fd>=0)::close(fd);fd=-1;}
void checked(int result,const char* message){if(result<0)throw std::runtime_error(std::string(message)+":"+std::strerror(errno));}
void spawn_checked(int result,const char* message){if(result)throw std::runtime_error(std::string(message)+":"+std::strerror(result));}
struct SpawnResources {
  int input[2]={-1,-1},output[2]={-1,-1};
  posix_spawn_file_actions_t actions;posix_spawnattr_t attributes;
  bool has_actions=false,has_attributes=false;
  ~SpawnResources(){
    for(auto& fd:input)close_fd(fd);for(auto& fd:output)close_fd(fd);
    if(has_actions)posix_spawn_file_actions_destroy(&actions);
    if(has_attributes)posix_spawnattr_destroy(&attributes);
  }
};
}
struct VlmProcess::Impl {
  AgentBackendConfig config;
  const std::thread::id owner=std::this_thread::get_id();
  pid_t pid=-1;
  int input=-1,output=-1;
  bool ready=false;
  std::string buffered;
  uint64_t last_activity=0,total_requests=0;
  unsigned process_requests=0;
  fs::path log_path;
  std::ofstream log;
  explicit Impl(AgentBackendConfig c):config(std::move(c)),log_path(fs::path(config.snapshot_root)/"vlm.log"){
    if(config.vlm_binary.empty()||config.llm_engine_dir.empty()||config.visual_engine_dir.empty()||config.plugin_path.empty()||
       config.snapshot_root.empty()||!config.vlm_max_requests||config.vlm_max_requests>64)
      throw std::invalid_argument("VLM_PROCESS_CONFIG");
  }
  void require_owner()const{if(owner!=std::this_thread::get_id())throw std::logic_error("VLM_PROCESS_OWNER");}
  void budget(uint64_t deadline,const std::function<bool()>& canceled){
    if((canceled&&canceled())||mono()>=deadline)throw std::runtime_error("VLM_CANCELED_OR_DEADLINE");
  }
  void record(const std::string& line){
    if(!log.is_open())log.open(log_path,std::ios::app);
    if(!log)throw std::runtime_error("VLM_LOG_OPEN");
    if(log.tellp()+std::streamoff(line.size()+1)>512*1024){
      log.close();std::error_code ignored;
      fs::remove(log_path.string()+".3",ignored);
      for(int i=2;i>=1;--i)if(fs::exists(log_path.string()+"."+std::to_string(i)))
        fs::rename(log_path.string()+"."+std::to_string(i),log_path.string()+"."+std::to_string(i+1));
      fs::rename(log_path,log_path.string()+".1");log.open(log_path,std::ios::trunc);
    }
    log<<line<<'\n';log.flush();
  }
  bool reaped()noexcept{
    if(pid<0)return true;int status=0;const auto result=waitpid(pid,&status,WNOHANG);
    if(result==pid || (result<0&&errno==ECHILD)){pid=-1;ready=false;return true;}
    return false;
  }
  void stop(bool force)noexcept{
    if(owner!=std::this_thread::get_id()){std::fputs("VLM_WRONG_DESTRUCTION_OWNER\n",stderr);std::_Exit(70);}
    if(input>=0)shutdown(input,SHUT_WR);close_fd(input);
    if(pid>=0 && !reaped()){
      if(force)kill(-pid,SIGTERM);
      const auto started=mono();bool term=force,killed=false;
      while(!reaped()){
        const auto elapsed=mono()-started;
        if(!term&&elapsed>=2000000000ULL){kill(-pid,SIGTERM);term=true;}
        if(!killed&&elapsed>=4000000000ULL){kill(-pid,SIGKILL);killed=true;}
        if(elapsed>=9000000000ULL){std::fputs("VLM_PROCESS_REAP_UNKNOWN\n",stderr);std::_Exit(70);}
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
      }
    }
    close_fd(output);ready=false;buffered.clear();process_requests=0;
  }
  void start(){
    require_owner();if(pid>=0)return;
    SpawnResources resources;
    checked(socketpair(AF_UNIX,SOCK_STREAM|SOCK_CLOEXEC,0,resources.input),"VLM_INPUT_SOCKET");
    checked(pipe2(resources.output,O_CLOEXEC),"VLM_OUTPUT_PIPE");
    spawn_checked(posix_spawn_file_actions_init(&resources.actions),"VLM_SPAWN_ACTIONS");resources.has_actions=true;
    spawn_checked(posix_spawnattr_init(&resources.attributes),"VLM_SPAWN_ATTRIBUTES");resources.has_attributes=true;
    spawn_checked(posix_spawn_file_actions_adddup2(&resources.actions,resources.input[1],STDIN_FILENO),"VLM_STDIN_DUP");
    spawn_checked(posix_spawn_file_actions_adddup2(&resources.actions,resources.output[1],STDOUT_FILENO),"VLM_STDOUT_DUP");
    spawn_checked(posix_spawn_file_actions_adddup2(&resources.actions,resources.output[1],STDERR_FILENO),"VLM_STDERR_DUP");
    for(int fd:{resources.input[0],resources.input[1],resources.output[0],resources.output[1]})
      if(fd>STDERR_FILENO)spawn_checked(posix_spawn_file_actions_addclose(&resources.actions,fd),"VLM_CHILD_FD_CLOSE");
    sigset_t empty,defaults;sigemptyset(&empty);sigemptyset(&defaults);
    for(int signal:{SIGINT,SIGTERM,SIGPIPE})sigaddset(&defaults,signal);
    spawn_checked(posix_spawnattr_setsigmask(&resources.attributes,&empty),"VLM_CHILD_SIGNAL_MASK");
    spawn_checked(posix_spawnattr_setsigdefault(&resources.attributes,&defaults),"VLM_CHILD_SIGNAL_DEFAULTS");
    spawn_checked(posix_spawnattr_setpgroup(&resources.attributes,0),"VLM_CHILD_PROCESS_GROUP");
    spawn_checked(posix_spawnattr_setflags(&resources.attributes,POSIX_SPAWN_SETPGROUP|POSIX_SPAWN_SETSIGMASK|POSIX_SPAWN_SETSIGDEF),"VLM_CHILD_FLAGS");
    std::vector<std::string> arguments={config.vlm_binary,config.llm_engine_dir,config.visual_engine_dir,config.snapshot_root};
    std::vector<char*> argv;for(auto& arg:arguments)argv.push_back(arg.data());argv.push_back(nullptr);
    std::vector<std::string> environment;
    for(char** entry=environ;*entry;++entry)if(std::string(*entry).rfind("EDGELLM_PLUGIN_PATH=",0)!=0)environment.emplace_back(*entry);
    environment.push_back("EDGELLM_PLUGIN_PATH="+config.plugin_path);
    std::vector<char*> env;for(auto& entry:environment)env.push_back(entry.data());env.push_back(nullptr);
    const int error=posix_spawn(&pid,config.vlm_binary.c_str(),&resources.actions,&resources.attributes,argv.data(),env.data());
    if(error){pid=-1;throw std::runtime_error("VLM_SPAWN:"+std::string(std::strerror(error)));}
    input=resources.input[0];resources.input[0]=-1;output=resources.output[0];resources.output[0]=-1;
    if(fcntl(input,F_SETFL,O_NONBLOCK)<0||fcntl(output,F_SETFL,O_NONBLOCK)<0){stop(true);throw std::runtime_error("VLM_NONBLOCKING_FDS");}
  }
  std::string read_line(uint64_t deadline,const std::function<bool()>& canceled){
    for(;;){
      budget(deadline,canceled);
      const auto newline=buffered.find('\n');
      if(newline!=std::string::npos){
        if(newline>65536)throw std::runtime_error("VLM_PROTOCOL_LINE_BUDGET");
        auto line=buffered.substr(0,newline);buffered.erase(0,newline+1);return line;}
      if(buffered.size()>65536)throw std::runtime_error("VLM_PROTOCOL_LINE_BUDGET");
      pollfd fd{output,POLLIN,0};const int result=poll(&fd,1,20);
      if(result<0&&errno==EINTR)continue;checked(result,"VLM_OUTPUT_POLL");
      if(!result){if(reaped())throw std::runtime_error("VLM_CHILD_EXITED");continue;}
      char bytes[4096];const auto count=read(output,bytes,sizeof(bytes));
      if(count>0){buffered.append(bytes,count);continue;}
      if(count<0&&(errno==EINTR||errno==EAGAIN))continue;
      throw std::runtime_error("VLM_OUTPUT_CLOSED");
    }
  }
  void send(const std::string& bytes,uint64_t deadline,const std::function<bool()>& canceled){
    std::size_t sent=0;
    while(sent<bytes.size()){
      budget(deadline,canceled);const auto n=::send(input,bytes.data()+sent,bytes.size()-sent,MSG_NOSIGNAL);
      if(n>0){sent+=n;continue;}
      if(n<0&&errno==EINTR)continue;
      if(n<0&&errno==EAGAIN){pollfd fd{input,POLLOUT,0};poll(&fd,1,20);continue;}
      throw std::runtime_error("VLM_INPUT_CLOSED");
    }
  }
};
VlmProcess::VlmProcess(AgentBackendConfig config):impl_(new Impl(std::move(config))){}
VlmProcess::~VlmProcess(){impl_->stop(false);}
void VlmProcess::stop(bool force)noexcept{impl_->stop(force);}
bool VlmProcess::alive()const noexcept{return impl_->pid>=0;}
uint64_t VlmProcess::last_activity_mono_ns()const noexcept{return impl_->last_activity;}
uint64_t VlmProcess::requests()const noexcept{return impl_->total_requests;}
VlmResponse VlmProcess::generate(const std::string& request_id,const std::string& system,const std::string& prompt,
  const std::string& image,unsigned tokens,uint64_t deadline,const std::function<bool()>& canceled,const std::function<void()>& submitted){
  auto& p=*impl_;p.require_owner();
  if(!tokens||tokens>192||prompt.size()>1600||system.size()>2500)throw std::invalid_argument("VLM_ADAPTER_BUDGET");
  const auto started=mono();NvtxScope range("agent/local-model-request");
  try{
    if(p.process_requests>=p.config.vlm_max_requests)p.stop(false);
    p.budget(deadline,canceled);p.start();
    while(!p.ready){auto row=p.read_line(deadline,canceled);p.record(row);
      if(row.rfind("EDGEVISION_VLM_READY ",0)==0){auto ready=Json::parse(row.substr(21));
        if(ready.at("schema_version")!=1)throw std::runtime_error("VLM_READY_SCHEMA");p.ready=true;}}
    Json messages=Json::array();if(!system.empty())messages.push_back({{"role","system"},{"text",system}});
    Json user={{"role","user"},{"text",prompt}};if(!image.empty())user["image_paths"]=Json::array({image});messages.push_back(user);
    const auto request=Json({{"request_id",request_id},{"messages",messages},{"max_tokens",tokens}}).dump()+"\n";
    if(request.size()>16384)throw std::runtime_error("VLM_REQUEST_BYTE_BUDGET");
    p.send(request,deadline,canceled);
    ++p.total_requests;
    if(submitted)submitted();
    for(;;){auto row=p.read_line(deadline,canceled);p.record(row);
      if(row.rfind("EDGEVISION_VLM_RESULT ",0)!=0)continue;
      const auto raw=row.substr(22);const auto result=Json::parse(raw);
      if(result.at("schema_version")!=1||result.at("request_id")!=request_id)throw std::runtime_error("VLM_RESPONSE_IDENTITY");
      if(!result.at("success").get<bool>())throw std::runtime_error("VLM_BACKEND:"+result.value("error_code",std::string("UNKNOWN")));
      auto text=result.at("text").get<std::string>();if(text.empty()||text.size()>8192)throw std::runtime_error("VLM_RESPONSE_BYTE_BUDGET");
      ++p.process_requests;p.last_activity=mono();return {std::move(text),raw,started,p.last_activity};
    }
  }catch(...){p.stop(true);throw;}
}
}
