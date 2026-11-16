#include "edgevision/snapshot_store.h"
#include <cassert>
#include <chrono>
#include <fstream>
#include <iostream>
using namespace edgevision;
int main(){
  namespace fs=std::filesystem;
  const auto root=fs::temp_directory_path()/std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
  fs::create_directory(root);std::ofstream(root/"preserve-user.txt")<<"owned by someone else";
  {
    SnapshotStore store(root,[](const Frame& f,const fs::path& p){std::ofstream out(p,std::ios::binary);out.write(reinterpret_cast<const char*>(f.data.data()),f.bytesused);},2,1024);
    Frame f;f.valid=true;f.data={1,2,3,4};f.bytesused=4;f.session_generation=7;f.frame_id=42;
    const auto first=store.save(f,1,10);auto lease=store.replay(first.id,7,11);
    f.data[0]=99;store.save(f,2,12);store.save(f,3,13);
    assert(store.size()==2 && store.retained_bytes()==16 && !fs::exists(first.path));
    assert(lease->data[0]==1 && lease->frame_id==42 && lease.state()==SlotState::Ready);
    bool missing=false;try{store.query(first.id,7,14);}catch(...){missing=true;}assert(missing);
    const auto latest=store.save(f,4,15);bool stale=false;try{store.query(latest.id,8,16);}catch(...){stale=true;}assert(stale);
    bool expired=false;try{store.query(latest.id,7,600000000016ULL);}catch(...){expired=true;}assert(expired);
    lease.release();
  }
  {SnapshotStore next(root,[](const Frame&,const fs::path&){},2,1024);assert(next.size()==0 && fs::exists(root/"preserve-user.txt"));}
  assert(fs::is_empty(root)==false);fs::remove(root/"preserve-user.txt");assert(fs::is_empty(root));fs::remove(root);
  std::cout<<"PASS bounded snapshot cache, exact owned replay, generation/expiry, eviction-safe lease; fixture codec only\n";
}
