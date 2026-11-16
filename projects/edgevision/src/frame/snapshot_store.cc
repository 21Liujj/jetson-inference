#include "edgevision/snapshot_store.h"
#include <chrono>
#include <sstream>
#include <stdexcept>

namespace edgevision {
namespace fs=std::filesystem;
namespace {
bool cache_name(const std::string& name){
  if(name.rfind("evs-",0)!=0 || name.size()>64 || name.size()<10)return false;
  const auto suffix=name.size()>=8 && name.compare(name.size()-8,8,".tmp.png")==0?8u:4u;
  if(name.substr(name.size()-4)!=".png")return false;
  for(std::size_t i=4;i<name.size()-suffix;++i)
    if(!((name[i]>='0'&&name[i]<='9')||(name[i]>='a'&&name[i]<='f')||name[i]=='-'))return false;
  return true;
}
}
SnapshotStore::SnapshotStore(fs::path root,Writer writer,unsigned capacity,std::size_t budget)
    :writer_(std::move(writer)),capacity_(capacity),budget_(budget){
  if(root.empty()||!writer_||!capacity||capacity>64||budget<1024||budget>256*1024*1024)
    throw std::invalid_argument("SNAPSHOT_STORE_BUDGET");
  fs::create_directories(root);root_=fs::canonical(root);
  instance_=std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
  // Only this cache's generated flat files are removed; never recurse through
  // a caller directory or follow a link out of the owned snapshot directory.
  for(const auto& entry:fs::directory_iterator(root_)){
    if(entry.is_symlink()||!entry.is_regular_file()||!cache_name(entry.path().filename().string()))continue;
    if(fs::canonical(entry.path()).parent_path()!=root_)throw std::runtime_error("SNAPSHOT_ROOT_ESCAPE");
    fs::remove(entry.path());
  }
}
void SnapshotStore::evict(){
  const auto& entry=entries_.front();fs::remove(entry.record.path);
  retained_bytes_-=entry.frame.data.size()+entry.record.encoded_bytes;entries_.pop_front();
}
SnapshotRecord SnapshotStore::save(const Frame& frame,uint64_t task,uint64_t at){
  if(!task||!at||!frame.valid||!frame.bytesused||frame.bytesused>4*1024*1024||frame.bytesused>frame.data.size())
    throw std::invalid_argument("SNAPSHOT_PAYLOAD");
  std::ostringstream id;id<<"evs-"<<std::hex<<instance_<<'-'<<task;
  if(id.str().size()>64)throw std::runtime_error("SNAPSHOT_ID_BUDGET");
  for(const auto& entry:entries_)if(entry.record.id==id.str())throw std::logic_error("SNAPSHOT_TASK_ALREADY_SAVED");
  const auto final=root_/(id.str()+".png"),temporary=root_/(id.str()+".tmp.png");
  if(fs::exists(final)||fs::exists(temporary))throw std::runtime_error("SNAPSHOT_FILE_COLLISION");
  try{
    writer_(frame,temporary);const auto encoded=fs::file_size(temporary);
    const auto bytes=std::size_t(frame.bytesused)+encoded;
    if(encoded>4*1024*1024||!encoded||bytes>budget_)throw std::runtime_error("SNAPSHOT_ENCODED_BUDGET");
    while(!entries_.empty()&&(entries_.size()>=capacity_||retained_bytes_+bytes>budget_))evict();
    Entry entry;entry.record={id.str(),final.string(),frame,task,at,std::size_t(encoded)};
    static_cast<FrameMeta&>(entry.frame)=frame;entry.frame.data.assign(frame.data.begin(),frame.data.begin()+frame.bytesused);entry.frame.valid=true;
    // Allocate the retained object before publishing its file. A failure cannot
    // leave a successful record pointing at a half-written image.
    entries_.push_back(std::move(entry));
    try{fs::rename(temporary,final);}catch(...){entries_.pop_back();throw;}
    retained_bytes_+=bytes;return entries_.back().record;
  }catch(...){std::error_code ignored;fs::remove(temporary,ignored);throw;}
}
const SnapshotStore::Entry& SnapshotStore::find(const std::string& id,uint64_t generation,uint64_t at)const{
  for(const auto& entry:entries_)if(entry.record.id==id){
    if(entry.record.frame.session_generation!=generation)throw std::runtime_error("STALE_SNAPSHOT_GENERATION");
    if(at<entry.record.saved_mono_ns||at-entry.record.saved_mono_ns>600000000000ULL)throw std::runtime_error("SNAPSHOT_EXPIRED");
    if(!fs::is_regular_file(entry.record.path))throw std::runtime_error("SNAPSHOT_FILE_MISSING");
    return entry;
  }
  throw std::runtime_error("SNAPSHOT_NOT_FOUND");
}
SnapshotRecord SnapshotStore::query(const std::string& id,uint64_t generation,uint64_t at)const{return find(id,generation,at).record;}
FrameLease SnapshotStore::replay(const std::string& id,uint64_t generation,uint64_t at)const{
  const auto& stored=find(id,generation,at);FramePool pool(1,stored.frame.bytesused);
  const int slot=pool.acquire_index();auto& frame=pool.slot_ref(slot);static_cast<FrameMeta&>(frame)=stored.frame;
  frame.data=stored.frame.data;frame.valid=true;FrameLease lease(&pool,slot);
  if(!lease.mark_ready(at))throw std::runtime_error("SNAPSHOT_LEASE_STATE");
  return lease; // independent shared storage survives the pool handle/cache eviction
}
}
